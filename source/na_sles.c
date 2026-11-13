/*
 * Copyright (C) 2026 Vita port contributors
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

/**
 * @file  na_sles.c
 * @brief The part of OpenSL ES the game uses, on SceAudio.
 *
 * The Vita OpenSL ES library hangs when the game destroys a player that never
 * played (CAudioPlayer_PreDestroy waits for a playback thread that was never
 * started). The game only needs: an engine and output mix, PCM buffer-queue
 * players (sound effects) and Android-fd players with MP3 data (music from the
 * OBB), with play state, volume, stereo position, playback rate, loop and
 * position. This file implements exactly that with one software mixer thread
 * on a SceAudio port, and mpg123 for the MP3s.
 */

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <unistd.h>

#include <psp2/audioout.h>
#include <psp2/io/fcntl.h>
#include <psp2/kernel/threadmgr.h>

#include <SLES/OpenSLES.h>
#include <SLES/OpenSLES_Android.h>
#include <mpg123.h>

// Android's extended PCM format (not in every OpenSLES_Android.h)
#ifndef SL_ANDROID_DATAFORMAT_PCM_EX
#define SL_ANDROID_DATAFORMAT_PCM_EX ((SLuint32) 0x00000004)
#define SL_ANDROID_PCM_REPRESENTATION_SIGNED_INT ((SLuint32) 0x00000001)
#define SL_ANDROID_PCM_REPRESENTATION_UNSIGNED_INT ((SLuint32) 0x00000002)
#define SL_ANDROID_PCM_REPRESENTATION_FLOAT ((SLuint32) 0x00000003)
typedef struct SLAndroidDataFormat_PCM_EX_ {
    SLuint32 formatType;
    SLuint32 numChannels;
    SLuint32 sampleRate;
    SLuint32 bitsPerSample;
    SLuint32 containerSize;
    SLuint32 channelMask;
    SLuint32 endianness;
    SLuint32 representation;
} SLAndroidDataFormat_PCM_EX;
#endif

#include "utils/logger.h"

#define MIX_RATE 48000
#define MIX_GRAIN 512
#define MAX_PLAYERS 96
#define MAX_QUEUE 16

enum { OBJ_ENGINE = 1, OBJ_OUTPUTMIX, OBJ_PLAYER };

typedef struct ng_obj ng_obj;

struct ng_obj {
    const struct SLObjectItf_ *obj_itf;
    const struct SLEngineItf_ *engine_itf;
    const struct SLPlayItf_ *play_itf;
    const struct SLAndroidSimpleBufferQueueItf_ *bq_itf;
    const struct SLVolumeItf_ *volume_itf;
    const struct SLSeekItf_ *seek_itf;
    const struct SLPlaybackRateItf_ *rate_itf;

    int kind;
    int in_use;

    // player
    int is_mp3;
    SLuint32 state;              // SL_PLAYSTATE_*
    int channels, rate;          // source format
    int is_float;                // 32-bit float PCM (SL_ANDROID_DATAFORMAT_PCM_EX), else 16-bit
    float pos;                   // position in the current buffer, in frames
    uint64_t frames_played;
    float gain_l, gain_r;
    SLmillibel level;
    SLboolean mute;
    SLboolean stereo_enabled;
    SLpermille stereo_pos;
    SLpermille playback_rate;    // 1000 = normal
    SLboolean loop;

    slPlayCallback play_cb;
    void *play_ctx;
    SLuint32 play_mask;

    // PCM buffer queue
    slAndroidSimpleBufferQueueCallback bq_cb;
    void *bq_ctx;
    const int16_t *queue[MAX_QUEUE];
    SLuint32 queue_bytes[MAX_QUEUE];
    int q_head, q_count;
    SLuint32 q_index;            // number of buffers consumed so far

    // MP3
    uint8_t *mp3_data;           // 32 KB read buffer
    size_t mp3_size;             // MP3 length in the OBB
    SceUID mp3_fd;
    int64_t mp3_off;
    size_t mp3_fed;
    mpg123_handle *mh;
    int16_t mp3_buf[4096 * 2];
    int mp3_frames, mp3_pos;
    int mp3_ended;
};

static ng_obj objs[MAX_PLAYERS + 4];
static SceKernelLwMutexWork mix_lock;
static int mix_started = 0;

#define OBJ_OF(self, field) ((ng_obj *)((uint8_t *)(self) - offsetof(ng_obj, field)))
#define LOCK() sceKernelLockLwMutex(&mix_lock, 1, NULL)
#define UNLOCK() sceKernelUnlockLwMutex(&mix_lock, 1)

static void update_gain(ng_obj *o) {
    float g = o->mute ? 0.0f : powf(10.0f, o->level / 2000.0f);
    float l = g, r = g;
    if (o->stereo_enabled) {
        float p = o->stereo_pos / 1000.0f; // -1 left .. 1 right
        if (p > 0) l *= 1.0f - p;
        else r *= 1.0f + p;
    }
    o->gain_l = l;
    o->gain_r = r;
}

/*
 * MP3 decoding (feeder mode: the whole MP3 is fed once, again for each loop)
 */

static int mp3_open(ng_obj *o) {
    if (o->mh) {
        mpg123_close(o->mh);
    } else {
        int err;
        o->mh = mpg123_new(NULL, &err);
        if (!o->mh) return -1;
        mpg123_param(o->mh, MPG123_FLAGS, MPG123_QUIET, 0.0);
        mpg123_format_none(o->mh);
        static const long rates[] = { 8000, 11025, 12000, 16000, 22050, 24000, 32000, 44100, 48000 };
        for (int i = 0; i < 9; i++)
            mpg123_format(o->mh, rates[i], MPG123_MONO | MPG123_STEREO, MPG123_ENC_SIGNED_16);
    }
    if (mpg123_open_feed(o->mh) != MPG123_OK) return -1;
    o->mp3_fed = 0; // streamed from the OBB by mp3_refill
    o->mp3_frames = o->mp3_pos = 0;
    o->mp3_ended = 0;
    return 0;
}

// Refills the decode buffer. Returns 0 at the end of the data.
#define MP3_CHUNK (32 * 1024)

static int mp3_refill(ng_obj *o) {
    for (int tries = 0; tries < 64; tries++) {
        size_t done = 0;
        int r = mpg123_read(o->mh, (unsigned char *) o->mp3_buf, sizeof(o->mp3_buf), &done);
        if (r == MPG123_NEW_FORMAT) {
            long rate;
            int ch, enc;
            mpg123_getformat(o->mh, &rate, &ch, &enc);
            o->rate = (int) rate;
            o->channels = ch;
        }
        if (done > 0) {
            o->mp3_frames = (int)(done / (2 * o->channels));
            o->mp3_pos = 0;
            return 1;
        }
        if (r == MPG123_NEED_MORE && o->mp3_fed < o->mp3_size && o->mp3_fd >= 0) {
            // Stream the next piece of the MP3 from the OBB
            size_t n = o->mp3_size - o->mp3_fed;
            if (n > MP3_CHUNK) n = MP3_CHUNK;
            int got = sceIoPread(o->mp3_fd, o->mp3_data, n, o->mp3_off + o->mp3_fed);
            if (got <= 0) return 0;
            mpg123_feed(o->mh, o->mp3_data, got);
            o->mp3_fed += got;
            continue;
        }
        if (r == MPG123_NEED_MORE || r == MPG123_DONE || r == MPG123_ERR) return 0;
    }
    return 0;
}

/*
 * Mixer
 */

typedef struct { slPlayCallback cb; SLPlayItf itf; void *ctx; SLuint32 ev; } pending_play;
typedef struct { slAndroidSimpleBufferQueueCallback cb; SLAndroidSimpleBufferQueueItf itf; void *ctx; } pending_bq;

static void mix_player(ng_obj *o, float *acc, int frames, pending_play *pp, int *npp, pending_bq *pb, int *npb) {
    float step = (float) o->rate / MIX_RATE * (o->playback_rate / 1000.0f);
    if (step <= 0) return;

    for (int i = 0; i < frames; i++) {
        const int16_t *src;
        int avail;
        if (o->is_mp3) {
            if (o->mp3_pos >= o->mp3_frames) {
                if (!mp3_refill(o)) {
                    if (o->loop && mp3_open(o) == 0 && mp3_refill(o)) {
                        // looped
                    } else {
                        o->state = SL_PLAYSTATE_PAUSED; // Android stops at the end
                        o->mp3_ended = 1;
                        if (o->play_cb && (o->play_mask & SL_PLAYEVENT_HEADATEND) && *npp < MAX_PLAYERS) {
                            pp[*npp].cb = o->play_cb; pp[*npp].itf = &o->play_itf;
                            pp[*npp].ctx = o->play_ctx; pp[*npp].ev = SL_PLAYEVENT_HEADATEND; (*npp)++;
                        }
                        return;
                    }
                }
                o->pos = 0;
            }
            src = o->mp3_buf;
            avail = o->mp3_frames;
            int idx = o->mp3_pos;
            float l = src[idx * o->channels], r = o->channels > 1 ? src[idx * o->channels + 1] : l;
            acc[i * 2] += l * o->gain_l;
            acc[i * 2 + 1] += r * o->gain_r;
            o->pos += step;
            int adv = (int) o->pos;
            o->pos -= adv;
            o->mp3_pos += adv;
            o->frames_played += adv;
            (void) avail;
        } else {
            if (o->q_count == 0) return;
            src = o->queue[o->q_head];
            avail = (int)(o->queue_bytes[o->q_head] / ((o->is_float ? 4 : 2) * o->channels));
            int idx = (int) o->pos;
            if (idx >= avail) {
                // buffer done
                o->q_head = (o->q_head + 1) % MAX_QUEUE;
                o->q_count--;
                o->q_index++;
                o->pos -= avail;
                if (o->pos < 0) o->pos = 0;
                if (o->bq_cb && *npb < MAX_PLAYERS * 2) {
                    pb[*npb].cb = o->bq_cb; pb[*npb].itf = &o->bq_itf; pb[*npb].ctx = o->bq_ctx; (*npb)++;
                }
                i--;
                continue;
            }
            float l, r;
            if (o->is_float) {
                const float *f = (const float *) src;
                l = f[idx * o->channels] * 32767.0f;
                r = o->channels > 1 ? f[idx * o->channels + 1] * 32767.0f : l;
            } else {
                l = src[idx * o->channels];
                r = o->channels > 1 ? src[idx * o->channels + 1] : l;
            }
            acc[i * 2] += l * o->gain_l;
            acc[i * 2 + 1] += r * o->gain_r;
            float before = o->pos;
            o->pos += step;
            o->frames_played += (uint64_t)((int) o->pos - (int) before);
        }
    }
}

static int mixer_thread(SceSize args, void *argp) {
    int port = sceAudioOutOpenPort(SCE_AUDIO_OUT_PORT_TYPE_BGM, MIX_GRAIN, MIX_RATE, SCE_AUDIO_OUT_MODE_STEREO);
    if (port < 0) {
        l_error("[SOUND] sceAudioOutOpenPort: 0x%x", port);
        return 0;
    }
    int vol[2] = { SCE_AUDIO_VOLUME_0DB, SCE_AUDIO_VOLUME_0DB };
    sceAudioOutSetVolume(port, SCE_AUDIO_VOLUME_FLAG_L_CH | SCE_AUDIO_VOLUME_FLAG_R_CH, vol);
    l_info("[SOUND] mixer running (port %d)", port);

    static float acc[MIX_GRAIN * 2];
    static int16_t out[2][MIX_GRAIN * 2];
    static pending_play pp[MAX_PLAYERS];
    static pending_bq pb[MAX_PLAYERS * 2];
    int cur = 0;

    while (1) {
        memset(acc, 0, sizeof(acc));
        int npp = 0, npb = 0;
        LOCK();
        for (int i = 0; i < MAX_PLAYERS + 4; i++) {
            ng_obj *o = &objs[i];
            if (o->in_use && o->kind == OBJ_PLAYER && o->state == SL_PLAYSTATE_PLAYING)
                mix_player(o, acc, MIX_GRAIN, pp, &npp, pb, &npb);
        }
        UNLOCK();

        // Callbacks outside the lock: they enqueue the next buffers
        for (int i = 0; i < npb; i++) pb[i].cb(pb[i].itf, pb[i].ctx);
        for (int i = 0; i < npp; i++) pp[i].cb(pp[i].itf, pp[i].ctx, pp[i].ev);

        for (int i = 0; i < MIX_GRAIN * 2; i++) {
            float v = acc[i];
            if (v > 32767.0f) v = 32767.0f;
            if (v < -32768.0f) v = -32768.0f;
            out[cur][i] = (int16_t) v;
        }
        sceAudioOutOutput(port, out[cur]);
        cur ^= 1;
    }
    return 0;
}

static void start_mixer(void) {
    if (mix_started) return;
    mix_started = 1;
    mpg123_init();
    SceUID t = sceKernelCreateThread("na_audio_mixer", mixer_thread, 0x10000100 - 0x10, 0x10000, 0, 0, NULL);
    sceKernelStartThread(t, 0, NULL);
}

/*
 * Interfaces
 */

static SLresult obj_Realize(SLObjectItf self, SLboolean async) { return SL_RESULT_SUCCESS; }
static SLresult obj_Resume(SLObjectItf self, SLboolean async) { return SL_RESULT_SUCCESS; }
static SLresult obj_GetState(SLObjectItf self, SLuint32 *state) { if (state) *state = SL_OBJECT_STATE_REALIZED; return SL_RESULT_SUCCESS; }
static SLresult obj_RegisterCallback(SLObjectItf self, slObjectCallback cb, void *ctx) { return SL_RESULT_SUCCESS; }
static void obj_AbortAsyncOperation(SLObjectItf self) {}
static SLresult obj_SetPriority(SLObjectItf self, SLint32 p, SLboolean pre) { return SL_RESULT_SUCCESS; }
static SLresult obj_GetPriority(SLObjectItf self, SLint32 *p, SLboolean *pre) { if (p) *p = 0; if (pre) *pre = SL_BOOLEAN_FALSE; return SL_RESULT_SUCCESS; }
static SLresult obj_SetLossOfControlInterfaces(SLObjectItf self, SLint16 n, SLInterfaceID *ids, SLboolean e) { return SL_RESULT_SUCCESS; }

static SLresult obj_GetInterface(SLObjectItf self, const SLInterfaceID iid, void *out) {
    ng_obj *o = OBJ_OF(self, obj_itf);
    void **itf = (void **) out;
    if (!itf) return SL_RESULT_PARAMETER_INVALID;
    if (iid == SL_IID_ENGINE && o->kind == OBJ_ENGINE) { *itf = &o->engine_itf; return SL_RESULT_SUCCESS; }
    if (o->kind == OBJ_PLAYER) {
        if (iid == SL_IID_PLAY) { *itf = &o->play_itf; return SL_RESULT_SUCCESS; }
        if (iid == SL_IID_VOLUME) { *itf = &o->volume_itf; return SL_RESULT_SUCCESS; }
        if (iid == SL_IID_SEEK) { *itf = &o->seek_itf; return SL_RESULT_SUCCESS; }
        if (iid == SL_IID_PLAYBACKRATE) { *itf = &o->rate_itf; return SL_RESULT_SUCCESS; }
        if ((iid == SL_IID_ANDROIDSIMPLEBUFFERQUEUE || iid == SL_IID_BUFFERQUEUE) && !o->is_mp3) {
            *itf = &o->bq_itf; return SL_RESULT_SUCCESS;
        }
    }
    *itf = NULL;
    return SL_RESULT_FEATURE_UNSUPPORTED;
}

static void obj_Destroy(SLObjectItf self) {
    ng_obj *o = OBJ_OF(self, obj_itf);
    LOCK();
    o->state = SL_PLAYSTATE_STOPPED;
    o->in_use = 0;
    UNLOCK();
    if (o->mh) { mpg123_delete(o->mh); o->mh = NULL; }
    free(o->mp3_data);
    o->mp3_data = NULL;
    if (o->is_mp3 && o->mp3_fd >= 0) sceIoClose(o->mp3_fd);
    o->mp3_fd = -1;
}

static const struct SLObjectItf_ obj_vtbl = {
    obj_Realize, obj_Resume, obj_GetState, obj_GetInterface, obj_RegisterCallback,
    obj_AbortAsyncOperation, obj_Destroy, obj_SetPriority, obj_GetPriority, obj_SetLossOfControlInterfaces
};

// Play
static SLresult play_SetPlayState(SLPlayItf self, SLuint32 state) {
    ng_obj *o = OBJ_OF(self, play_itf);
    LOCK();
    if (state == SL_PLAYSTATE_STOPPED) {
        o->pos = 0;
        o->frames_played = 0;
        if (o->is_mp3) mp3_open(o);
    } else if (state == SL_PLAYSTATE_PLAYING && o->is_mp3 && o->mp3_ended) {
        mp3_open(o);
    }
    o->state = state;
    UNLOCK();
    return SL_RESULT_SUCCESS;
}
static SLresult play_GetPlayState(SLPlayItf self, SLuint32 *state) { *state = OBJ_OF(self, play_itf)->state; return SL_RESULT_SUCCESS; }
static SLresult play_GetDuration(SLPlayItf self, SLmillisecond *ms) { *ms = SL_TIME_UNKNOWN; return SL_RESULT_SUCCESS; }
static SLresult play_GetPosition(SLPlayItf self, SLmillisecond *ms) {
    ng_obj *o = OBJ_OF(self, play_itf);
    *ms = o->rate ? (SLmillisecond)(o->frames_played * 1000 / o->rate) : 0;
    return SL_RESULT_SUCCESS;
}
static SLresult play_RegisterCallback(SLPlayItf self, slPlayCallback cb, void *ctx) {
    ng_obj *o = OBJ_OF(self, play_itf);
    o->play_cb = cb; o->play_ctx = ctx;
    return SL_RESULT_SUCCESS;
}
static SLresult play_SetCallbackEventsMask(SLPlayItf self, SLuint32 m) { OBJ_OF(self, play_itf)->play_mask = m; return SL_RESULT_SUCCESS; }
static SLresult play_GetCallbackEventsMask(SLPlayItf self, SLuint32 *m) { *m = OBJ_OF(self, play_itf)->play_mask; return SL_RESULT_SUCCESS; }
static SLresult play_SetMarkerPosition(SLPlayItf self, SLmillisecond p) { return SL_RESULT_SUCCESS; }
static SLresult play_ClearMarkerPosition(SLPlayItf self) { return SL_RESULT_SUCCESS; }
static SLresult play_GetMarkerPosition(SLPlayItf self, SLmillisecond *p) { *p = 0; return SL_RESULT_SUCCESS; }
static SLresult play_SetPositionUpdatePeriod(SLPlayItf self, SLmillisecond p) { return SL_RESULT_SUCCESS; }
static SLresult play_GetPositionUpdatePeriod(SLPlayItf self, SLmillisecond *p) { *p = 1000; return SL_RESULT_SUCCESS; }

static const struct SLPlayItf_ play_vtbl = {
    play_SetPlayState, play_GetPlayState, play_GetDuration, play_GetPosition, play_RegisterCallback,
    play_SetCallbackEventsMask, play_GetCallbackEventsMask, play_SetMarkerPosition, play_ClearMarkerPosition,
    play_GetMarkerPosition, play_SetPositionUpdatePeriod, play_GetPositionUpdatePeriod
};

// Android simple buffer queue
static SLresult bq_Enqueue(SLAndroidSimpleBufferQueueItf self, const void *buf, SLuint32 size) {
    ng_obj *o = OBJ_OF(self, bq_itf);
    SLresult r = SL_RESULT_SUCCESS;
    LOCK();
    if (o->q_count >= MAX_QUEUE) r = SL_RESULT_BUFFER_INSUFFICIENT;
    else {
        int slot = (o->q_head + o->q_count) % MAX_QUEUE;
        o->queue[slot] = buf;
        o->queue_bytes[slot] = size;
        o->q_count++;
    }
    UNLOCK();
    return r;
}
static SLresult bq_Clear(SLAndroidSimpleBufferQueueItf self) {
    ng_obj *o = OBJ_OF(self, bq_itf);
    LOCK();
    o->q_count = 0;
    o->pos = 0;
    UNLOCK();
    return SL_RESULT_SUCCESS;
}
static SLresult bq_GetState(SLAndroidSimpleBufferQueueItf self, SLAndroidSimpleBufferQueueState *st) {
    ng_obj *o = OBJ_OF(self, bq_itf);
    st->count = o->q_count;
    st->index = o->q_index;
    return SL_RESULT_SUCCESS;
}
static SLresult bq_RegisterCallback(SLAndroidSimpleBufferQueueItf self, slAndroidSimpleBufferQueueCallback cb, void *ctx) {
    ng_obj *o = OBJ_OF(self, bq_itf);
    o->bq_cb = cb; o->bq_ctx = ctx;
    return SL_RESULT_SUCCESS;
}
static const struct SLAndroidSimpleBufferQueueItf_ bq_vtbl = { bq_Enqueue, bq_Clear, bq_GetState, bq_RegisterCallback };

// Volume
static SLresult vol_SetVolumeLevel(SLVolumeItf self, SLmillibel l) { ng_obj *o = OBJ_OF(self, volume_itf); o->level = l; update_gain(o); return SL_RESULT_SUCCESS; }
static SLresult vol_GetVolumeLevel(SLVolumeItf self, SLmillibel *l) { *l = OBJ_OF(self, volume_itf)->level; return SL_RESULT_SUCCESS; }
static SLresult vol_GetMaxVolumeLevel(SLVolumeItf self, SLmillibel *l) { *l = 0; return SL_RESULT_SUCCESS; }
static SLresult vol_SetMute(SLVolumeItf self, SLboolean m) { ng_obj *o = OBJ_OF(self, volume_itf); o->mute = m; update_gain(o); return SL_RESULT_SUCCESS; }
static SLresult vol_GetMute(SLVolumeItf self, SLboolean *m) { *m = OBJ_OF(self, volume_itf)->mute; return SL_RESULT_SUCCESS; }
static SLresult vol_EnableStereoPosition(SLVolumeItf self, SLboolean e) { ng_obj *o = OBJ_OF(self, volume_itf); o->stereo_enabled = e; update_gain(o); return SL_RESULT_SUCCESS; }
static SLresult vol_IsEnabledStereoPosition(SLVolumeItf self, SLboolean *e) { *e = OBJ_OF(self, volume_itf)->stereo_enabled; return SL_RESULT_SUCCESS; }
static SLresult vol_SetStereoPosition(SLVolumeItf self, SLpermille p) { ng_obj *o = OBJ_OF(self, volume_itf); o->stereo_pos = p; update_gain(o); return SL_RESULT_SUCCESS; }
static SLresult vol_GetStereoPosition(SLVolumeItf self, SLpermille *p) { *p = OBJ_OF(self, volume_itf)->stereo_pos; return SL_RESULT_SUCCESS; }
static const struct SLVolumeItf_ vol_vtbl = {
    vol_SetVolumeLevel, vol_GetVolumeLevel, vol_GetMaxVolumeLevel, vol_SetMute, vol_GetMute,
    vol_EnableStereoPosition, vol_IsEnabledStereoPosition, vol_SetStereoPosition, vol_GetStereoPosition
};

// Seek
static SLresult seek_SetPosition(SLSeekItf self, SLmillisecond pos, SLuint32 mode) {
    ng_obj *o = OBJ_OF(self, seek_itf);
    if (pos == 0) {
        LOCK();
        o->pos = 0;
        o->frames_played = 0;
        if (o->is_mp3) mp3_open(o);
        UNLOCK();
    }
    return SL_RESULT_SUCCESS;
}
static SLresult seek_SetLoop(SLSeekItf self, SLboolean loop, SLmillisecond s, SLmillisecond e) { OBJ_OF(self, seek_itf)->loop = loop; return SL_RESULT_SUCCESS; }
static SLresult seek_GetLoop(SLSeekItf self, SLboolean *loop, SLmillisecond *s, SLmillisecond *e) {
    *loop = OBJ_OF(self, seek_itf)->loop; if (s) *s = 0; if (e) *e = SL_TIME_UNKNOWN; return SL_RESULT_SUCCESS;
}
static const struct SLSeekItf_ seek_vtbl = { seek_SetPosition, seek_SetLoop, seek_GetLoop };

// Playback rate
static SLresult rate_SetRate(SLPlaybackRateItf self, SLpermille r) { OBJ_OF(self, rate_itf)->playback_rate = r > 0 ? r : 1000; return SL_RESULT_SUCCESS; }
static SLresult rate_GetRate(SLPlaybackRateItf self, SLpermille *r) { *r = OBJ_OF(self, rate_itf)->playback_rate; return SL_RESULT_SUCCESS; }
static SLresult rate_SetPropertyConstraints(SLPlaybackRateItf self, SLuint32 c) { return SL_RESULT_SUCCESS; }
static SLresult rate_GetProperties(SLPlaybackRateItf self, SLuint32 *p) { *p = SL_RATEPROP_NOPITCHCORAUDIO; return SL_RESULT_SUCCESS; }
static SLresult rate_GetCapabilitiesOfRate(SLPlaybackRateItf self, SLpermille r, SLuint32 *c) { *c = SL_RATEPROP_NOPITCHCORAUDIO; return SL_RESULT_SUCCESS; }
static SLresult rate_GetRateRange(SLPlaybackRateItf self, SLuint8 i, SLpermille *min, SLpermille *max, SLpermille *step, SLuint32 *caps) {
    // One range only: the game loops over the indices until this fails
    if (i > 0) return SL_RESULT_PARAMETER_INVALID;
    *min = 500; *max = 2000; *step = 1; *caps = SL_RATEPROP_NOPITCHCORAUDIO; return SL_RESULT_SUCCESS;
}
static const struct SLPlaybackRateItf_ rate_vtbl = {
    rate_SetRate, rate_GetRate, rate_SetPropertyConstraints, rate_GetProperties, rate_GetCapabilitiesOfRate, rate_GetRateRange
};

static ng_obj *alloc_obj(int kind) {
    LOCK();
    for (int i = 0; i < MAX_PLAYERS + 4; i++) {
        if (!objs[i].in_use) {
            ng_obj *o = &objs[i];
            memset(o, 0, sizeof(*o));
            o->in_use = 1;
            o->kind = kind;
            o->obj_itf = &obj_vtbl;
            o->engine_itf = NULL;
            o->play_itf = &play_vtbl;
            o->bq_itf = &bq_vtbl;
            o->volume_itf = &vol_vtbl;
            o->seek_itf = &seek_vtbl;
            o->rate_itf = &rate_vtbl;
            o->state = SL_PLAYSTATE_STOPPED;
            o->playback_rate = 1000;
            o->channels = 1;
            o->rate = 44100;
            update_gain(o);
            UNLOCK();
            return o;
        }
    }
    UNLOCK();
    return NULL;
}

// Engine
static SLresult eng_CreateAudioPlayer(SLEngineItf self, SLObjectItf *player, SLDataSource *src, SLDataSink *sink,
                                      SLuint32 n, const SLInterfaceID *ids, const SLboolean *req) {
    if (!player || !src || !src->pLocator) return SL_RESULT_PARAMETER_INVALID;
    SLuint32 loc = *(SLuint32 *) src->pLocator;
    ng_obj *o = alloc_obj(OBJ_PLAYER);
    if (!o) return SL_RESULT_MEMORY_FAILURE;

    if (loc == SL_DATALOCATOR_ANDROIDSIMPLEBUFFERQUEUE || loc == SL_DATALOCATOR_BUFFERQUEUE) {
        SLDataFormat_PCM *fmt = (SLDataFormat_PCM *) src->pFormat;
        if (fmt && fmt->formatType == SL_DATAFORMAT_PCM) {
            o->channels = fmt->numChannels ? fmt->numChannels : 1;
            o->rate = fmt->samplesPerSec / 1000;
            if (fmt->bitsPerSample != 16)
                l_warn("[SOUND] %u-bit PCM isn't supported", (unsigned) fmt->bitsPerSample);
        } else if (fmt && fmt->formatType == SL_ANDROID_DATAFORMAT_PCM_EX) {
            // FMOD on newer Android: float or 16-bit PCM
            SLAndroidDataFormat_PCM_EX *ex = (SLAndroidDataFormat_PCM_EX *) fmt;
            o->channels = ex->numChannels ? ex->numChannels : 1;
            o->rate = ex->sampleRate / 1000;
            o->is_float = ex->representation == SL_ANDROID_PCM_REPRESENTATION_FLOAT;
            l_info("[SOUND] PCM_EX player: %d ch, %d Hz, %u bit%s", o->channels, o->rate,
                   (unsigned) ex->bitsPerSample, o->is_float ? " float" : "");
        }
    } else if (loc == SL_DATALOCATOR_ANDROIDFD) {
        SLDataLocator_AndroidFD *fd = (SLDataLocator_AndroidFD *) src->pLocator;
        // Android-fd players (from the Ninjago port). Unity/FMOD only uses
        // buffer queues; should an fd player appear, the data is assumed to
        // be inside the APK at the given offset.
        o->mp3_size = 0;
        o->mp3_fd = -1;
        if (fd->length > 0) {
            // Streamed in pieces by the mixer: reading a whole song here
            // froze the game for about a second whenever the music changed.
            o->mp3_data = malloc(MP3_CHUNK);
            o->mp3_fd = sceIoOpen(DATA_PATH "base.apk", SCE_O_RDONLY, 0);
            o->mp3_off = fd->offset;
            if (o->mp3_data && o->mp3_fd >= 0) o->mp3_size = (size_t) fd->length;
        }
        o->is_mp3 = 1;
        if (!o->mp3_size || mp3_open(o) < 0) {
            l_warn("[SOUND] couldn't read the MP3 (fd %d, offset %lld, length %lld)", (int) fd->fd,
                   (long long) fd->offset, (long long) fd->length);
        }
        static int logged = 0;
        if (logged++ < 20) l_info("[SOUND] music player: %u bytes of MP3", (unsigned) o->mp3_size);
    } else {
        l_warn("[SOUND] unsupported data locator 0x%x", (unsigned) loc);
    }

    start_mixer();
    *player = (SLObjectItf) &o->obj_itf;
    return SL_RESULT_SUCCESS;
}

static SLresult eng_CreateOutputMix(SLEngineItf self, SLObjectItf *mix, SLuint32 n, const SLInterfaceID *ids, const SLboolean *req) {
    ng_obj *o = alloc_obj(OBJ_OUTPUTMIX);
    if (!o) return SL_RESULT_MEMORY_FAILURE;
    *mix = (SLObjectItf) &o->obj_itf;
    return SL_RESULT_SUCCESS;
}

static SLresult eng_unsupported() { return SL_RESULT_FEATURE_UNSUPPORTED; }
static SLresult eng_QueryNumSupportedInterfaces(SLEngineItf self, SLuint32 type, SLuint32 *n) { *n = 0; return SL_RESULT_SUCCESS; }
static SLresult eng_QueryNumSupportedExtensions(SLEngineItf self, SLuint32 *n) { *n = 0; return SL_RESULT_SUCCESS; }

static const struct SLEngineItf_ eng_vtbl = {
    .CreateLEDDevice = (void *) eng_unsupported,
    .CreateVibraDevice = (void *) eng_unsupported,
    .CreateAudioPlayer = eng_CreateAudioPlayer,
    .CreateAudioRecorder = (void *) eng_unsupported,
    .CreateMidiPlayer = (void *) eng_unsupported,
    .CreateListener = (void *) eng_unsupported,
    .Create3DGroup = (void *) eng_unsupported,
    .CreateOutputMix = eng_CreateOutputMix,
    .CreateMetadataExtractor = (void *) eng_unsupported,
    .CreateExtensionObject = (void *) eng_unsupported,
    .QueryNumSupportedInterfaces = eng_QueryNumSupportedInterfaces,
    .QuerySupportedInterfaces = (void *) eng_unsupported,
    .QueryNumSupportedExtensions = eng_QueryNumSupportedExtensions,
    .QuerySupportedExtension = (void *) eng_unsupported,
    .IsExtensionSupported = (void *) eng_unsupported,
};

SLresult ng_slCreateEngine(SLObjectItf *engine) {
    static int inited = 0;
    if (!inited) {
        sceKernelCreateLwMutex(&mix_lock, "ng_audio", SCE_KERNEL_MUTEX_ATTR_RECURSIVE, 0, NULL);
        inited = 1;
    }
    ng_obj *o = alloc_obj(OBJ_ENGINE);
    if (!o) return SL_RESULT_MEMORY_FAILURE;
    o->engine_itf = &eng_vtbl;
    *engine = (SLObjectItf) &o->obj_itf;
    l_info("[SOUND] engine created (own OpenSL ES)");
    return SL_RESULT_SUCCESS;
}
