/*
 * Copyright (C) 2026 Vita port contributors
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

/**
 * @file  na_aaudio.c
 * @brief AAudio (Android's audio output API) on SceAudio.
 *
 * Unity 6's FMOD only uses AAudio on Android. It opens one output stream,
 * usually with a data callback: a thread here calls the callback for each
 * block, converts float or 16-bit samples to 16-bit stereo and feeds a
 * SceAudio port (blocking, which paces the thread). Without a callback the
 * game writes with AAudioStream_write().
 */

#include "na_aaudio.h"
#include "na_threads.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <psp2/audioout.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/kernel/cpu.h>
#include <psp2/kernel/processmgr.h>

#include "utils/logger.h"

#define AAUDIO_OK 0
#define AAUDIO_ERROR_ILLEGAL_ARGUMENT (-898)
#define AAUDIO_ERROR_INVALID_STATE (-895)
#define AAUDIO_ERROR_UNAVAILABLE (-889)
#define AAUDIO_ERROR_TIMEOUT (-885)
#define AAUDIO_UNSPECIFIED 0
#define AAUDIO_FORMAT_PCM_I16 1
#define AAUDIO_FORMAT_PCM_FLOAT 2
#define AAUDIO_DIRECTION_OUTPUT 0
#define AAUDIO_SHARING_MODE_SHARED 1
#define AAUDIO_PERFORMANCE_MODE_NONE 10

enum {
    ST_UNINITIALIZED = 0, ST_UNKNOWN, ST_OPEN, ST_STARTING, ST_STARTED, ST_PAUSING, ST_PAUSED,
    ST_FLUSHING, ST_FLUSHED, ST_STOPPING, ST_STOPPED, ST_CLOSING, ST_CLOSED, ST_DISCONNECTED
};

#define RATE 48000
#define GRAIN 1024 // frames per SceAudio block (multiple of 64); 21 ms, room for the mixer when the CPU is busy

typedef int32_t (*data_cb_t)(void *stream, void *user, void *audio, int32_t frames);
typedef void (*error_cb_t)(void *stream, void *user, int32_t error);

typedef struct {
    int32_t rate, channels, format, direction, frames_per_cb, perf_mode, sharing, capacity;
    data_cb_t data_cb;
    void *data_user;
    error_cb_t error_cb;
    void *error_user;
} builder;

typedef struct {
    builder cfg;
    volatile int32_t state;
    int port;
    SceUID thread;
    volatile int quit;
    int64_t frames_written;
    int32_t buffer_size;
    // write() mode: one block being filled
    int16_t *wbuf;
    int32_t wfill;
} stream;

static int bytes_per_sample(int format) {
    return format == AAUDIO_FORMAT_PCM_FLOAT ? 4 : 2;
}

// src (cfg format/channels) -> 16-bit stereo
static void convert(const stream *s, const void *src, int16_t *dst, int frames) {
    int ch = s->cfg.channels;
    if (s->cfg.format == AAUDIO_FORMAT_PCM_FLOAT) {
        const float *f = src;
        for (int i = 0; i < frames; i++) {
            float l = f[i * ch], r = ch > 1 ? f[i * ch + 1] : l;
            if (l > 1.0f) l = 1.0f; else if (l < -1.0f) l = -1.0f;
            if (r > 1.0f) r = 1.0f; else if (r < -1.0f) r = -1.0f;
            dst[i * 2] = (int16_t)(l * 32767.0f);
            dst[i * 2 + 1] = (int16_t)(r * 32767.0f);
        }
    } else {
        const int16_t *p = src;
        for (int i = 0; i < frames; i++) {
            dst[i * 2] = p[i * ch];
            dst[i * 2 + 1] = ch > 1 ? p[i * ch + 1] : p[i * ch];
        }
    }
}

static int audio_thread(SceSize args, void *argp) {
    stream *s = *(stream **)argp;
    na_thread_register("aaudio", (void *)audio_thread);
    int bps = bytes_per_sample(s->cfg.format) * s->cfg.channels;
    void *cbuf = calloc(GRAIN, bps);
    int16_t *out = calloc(GRAIN * 2, sizeof(int16_t));
    l_info("[AAudio] thread running (%d Hz, %d ch, format %d)", s->cfg.rate, s->cfg.channels, s->cfg.format);
    while (!s->quit) {
        if (s->state != ST_STARTED) {
            sceKernelDelayThread(5000);
            continue;
        }
        int32_t r = s->cfg.data_cb ? s->cfg.data_cb(s, s->cfg.data_user, cbuf, GRAIN) : 1;
        if (r != 0) { // AAUDIO_CALLBACK_RESULT_STOP
            s->state = ST_STOPPED;
            continue;
        }
        convert(s, cbuf, out, GRAIN);
        sceAudioOutOutput(s->port, out);
        s->frames_written += GRAIN;
    }
    free(cbuf);
    free(out);
    return 0;
}

/*
 * Builder
 */
static int32_t createStreamBuilder(builder **out) {
    builder *b = calloc(1, sizeof(*b));
    b->rate = AAUDIO_UNSPECIFIED;
    b->channels = AAUDIO_UNSPECIFIED;
    b->format = AAUDIO_UNSPECIFIED;
    b->sharing = AAUDIO_SHARING_MODE_SHARED;
    b->perf_mode = AAUDIO_PERFORMANCE_MODE_NONE;
    *out = b;
    return AAUDIO_OK;
}

static void b_setSampleRate(builder *b, int32_t v) { b->rate = v; }
static void b_setChannelCount(builder *b, int32_t v) { b->channels = v; }
static void b_setFormat(builder *b, int32_t v) { b->format = v; }
static void b_setDirection(builder *b, int32_t v) { b->direction = v; }
static void b_setDeviceId(builder *b, int32_t v) {}
static void b_setSharingMode(builder *b, int32_t v) { b->sharing = v; }
static void b_setPerformanceMode(builder *b, int32_t v) { b->perf_mode = v; }
static void b_setBufferCapacityInFrames(builder *b, int32_t v) { b->capacity = v; }
static void b_setFramesPerDataCallback(builder *b, int32_t v) { b->frames_per_cb = v; }
static void b_setUsage(builder *b, int32_t v) {}
static void b_setContentType(builder *b, int32_t v) {}
static void b_setInputPreset(builder *b, int32_t v) {}
static void b_setSessionId(builder *b, int32_t v) {}
static void b_setAllowedCapturePolicy(builder *b, int32_t v) {}
static void b_setChannelMask(builder *b, uint32_t v) {}
static void b_setDataCallback(builder *b, data_cb_t cb, void *user) { b->data_cb = cb; b->data_user = user; }
static void b_setErrorCallback(builder *b, error_cb_t cb, void *user) { b->error_cb = cb; b->error_user = user; }
static int32_t b_delete(builder *b) { free(b); return AAUDIO_OK; }

static int32_t b_openStream(builder *b, stream **out) {
    if (b->direction != AAUDIO_DIRECTION_OUTPUT) {
        l_warn("[AAudio] input streams are not supported");
        return AAUDIO_ERROR_UNAVAILABLE;
    }
    stream *s = calloc(1, sizeof(*s));
    s->cfg = *b;
    // The Vita mixes at 48 kHz; report what we really do
    s->cfg.rate = RATE;
    if (s->cfg.channels <= 0 || s->cfg.channels > 2) s->cfg.channels = 2;
    if (s->cfg.format != AAUDIO_FORMAT_PCM_I16) s->cfg.format = AAUDIO_FORMAT_PCM_FLOAT;
    s->cfg.frames_per_cb = GRAIN;
    s->buffer_size = GRAIN * 2;
    s->port = sceAudioOutOpenPort(SCE_AUDIO_OUT_PORT_TYPE_BGM, GRAIN, RATE, SCE_AUDIO_OUT_MODE_STEREO);
    if (s->port < 0) {
        l_error("[AAudio] sceAudioOutOpenPort: 0x%x", s->port);
        free(s);
        return AAUDIO_ERROR_UNAVAILABLE;
    }
    int vol[2] = { SCE_AUDIO_VOLUME_0DB, SCE_AUDIO_VOLUME_0DB };
    sceAudioOutSetVolume(s->port, SCE_AUDIO_VOLUME_FLAG_L_CH | SCE_AUDIO_VOLUME_FLAG_R_CH, vol);
    s->state = ST_OPEN;
    if (s->cfg.data_cb) {
        // own core (the third): while a scene loads, the main thread and the
        // loading jobs keep the other cores busy and the music stuttered
        s->thread = sceKernelCreateThread("aaudio", audio_thread, 0x10000100 - 10, 0x10000, 0,
                                          SCE_KERNEL_CPU_MASK_USER_2, NULL);
        stream *arg = s;
        sceKernelStartThread(s->thread, sizeof(arg), &arg);
    } else {
        s->wbuf = calloc(GRAIN * 2, sizeof(int16_t));
    }
    l_info("[AAudio] stream opened: %d Hz, %d ch, format %d, %s", s->cfg.rate, s->cfg.channels, s->cfg.format,
           s->cfg.data_cb ? "callback" : "write");
    *out = s;
    return AAUDIO_OK;
}

/*
 * Stream
 */
static int32_t s_requestStart(stream *s) { s->state = ST_STARTED; return AAUDIO_OK; }
static int32_t s_requestPause(stream *s) { s->state = ST_PAUSED; return AAUDIO_OK; }
static int32_t s_requestFlush(stream *s) { s->state = ST_FLUSHED; return AAUDIO_OK; }
static int32_t s_requestStop(stream *s) { s->state = ST_STOPPED; return AAUDIO_OK; }

static int32_t s_close(stream *s) {
    s->state = ST_CLOSED;
    s->quit = 1;
    if (s->thread > 0) {
        sceKernelWaitThreadEnd(s->thread, NULL, NULL);
        sceKernelDeleteThread(s->thread);
    }
    sceAudioOutReleasePort(s->port);
    free(s->wbuf);
    free(s);
    return AAUDIO_OK;
}

static int32_t s_getState(stream *s) { return s->state; }

static int32_t s_waitForStateChange(stream *s, int32_t input, int32_t *next, int64_t timeout_ns) {
    int64_t waited = 0;
    while (s->state == input && waited < timeout_ns) {
        sceKernelDelayThread(1000);
        waited += 1000000;
    }
    if (next) *next = s->state;
    return s->state == input ? AAUDIO_ERROR_TIMEOUT : AAUDIO_OK;
}

static int32_t s_write(stream *s, const void *buf, int32_t frames, int64_t timeout_ns) {
    if (!s->wbuf) return AAUDIO_ERROR_INVALID_STATE;
    int bps = bytes_per_sample(s->cfg.format) * s->cfg.channels;
    const uint8_t *p = buf;
    int32_t left = frames;
    while (left > 0) {
        int32_t n = GRAIN - s->wfill;
        if (n > left) n = left;
        convert(s, p, s->wbuf + s->wfill * 2, n);
        s->wfill += n;
        p += n * bps;
        left -= n;
        if (s->wfill == GRAIN) {
            sceAudioOutOutput(s->port, s->wbuf);
            s->frames_written += GRAIN;
            s->wfill = 0;
        }
    }
    return frames;
}

static int32_t s_getSampleRate(stream *s) { return s->cfg.rate; }
static int32_t s_getChannelCount(stream *s) { return s->cfg.channels; }
static int32_t s_getSamplesPerFrame(stream *s) { return s->cfg.channels; }
static int32_t s_getFormat(stream *s) { return s->cfg.format; }
static int32_t s_getDirection(stream *s) { return AAUDIO_DIRECTION_OUTPUT; }
static int32_t s_getDeviceId(stream *s) { return 1; }
static int32_t s_getSharingMode(stream *s) { return AAUDIO_SHARING_MODE_SHARED; }
static int32_t s_getPerformanceMode(stream *s) { return s->cfg.perf_mode; }
static int32_t s_getFramesPerBurst(stream *s) { return GRAIN; }
static int32_t s_getFramesPerDataCallback(stream *s) { return GRAIN; }
static int32_t s_getBufferSizeInFrames(stream *s) { return s->buffer_size; }
static int32_t s_setBufferSizeInFrames(stream *s, int32_t n) { s->buffer_size = n < GRAIN ? GRAIN : n; return s->buffer_size; }
static int32_t s_getBufferCapacityInFrames(stream *s) { return GRAIN * 4; }
static int32_t s_getXRunCount(stream *s) { return 0; }
static int s_isMMapUsed(stream *s) { return 0; }
static int64_t s_getFramesWritten(stream *s) { return s->frames_written; }
static int64_t s_getFramesRead(stream *s) { return s->frames_written; }
static int32_t s_getUsage(stream *s) { return 1; }
static int32_t s_getContentType(stream *s) { return 2; }
static int32_t s_getSessionId(stream *s) { return -1; }

static int32_t s_getTimestamp(stream *s, int32_t clock, int64_t *frame, int64_t *time_ns) {
    if (frame) *frame = s->frames_written;
    if (time_ns) *time_ns = (int64_t)sceKernelGetProcessTimeWide() * 1000;
    return AAUDIO_OK;
}

static const char *convertResultToText(int32_t r) { return r == 0 ? "AAUDIO_OK" : "AAUDIO_ERROR"; }
static const char *convertStreamStateToText(int32_t s) { return "AAUDIO_STREAM_STATE"; }

#define E(n, f) { n, (void *)&f }

static const struct { const char *name; void *fn; } aaudio_table[] = {
    E("AAudio_createStreamBuilder", createStreamBuilder),
    E("AAudio_convertResultToText", convertResultToText),
    E("AAudio_convertStreamStateToText", convertStreamStateToText),
    E("AAudioStreamBuilder_setSampleRate", b_setSampleRate),
    E("AAudioStreamBuilder_setChannelCount", b_setChannelCount),
    E("AAudioStreamBuilder_setSamplesPerFrame", b_setChannelCount),
    E("AAudioStreamBuilder_setFormat", b_setFormat),
    E("AAudioStreamBuilder_setDirection", b_setDirection),
    E("AAudioStreamBuilder_setDeviceId", b_setDeviceId),
    E("AAudioStreamBuilder_setSharingMode", b_setSharingMode),
    E("AAudioStreamBuilder_setPerformanceMode", b_setPerformanceMode),
    E("AAudioStreamBuilder_setBufferCapacityInFrames", b_setBufferCapacityInFrames),
    E("AAudioStreamBuilder_setFramesPerDataCallback", b_setFramesPerDataCallback),
    E("AAudioStreamBuilder_setUsage", b_setUsage),
    E("AAudioStreamBuilder_setContentType", b_setContentType),
    E("AAudioStreamBuilder_setInputPreset", b_setInputPreset),
    E("AAudioStreamBuilder_setSessionId", b_setSessionId),
    E("AAudioStreamBuilder_setAllowedCapturePolicy", b_setAllowedCapturePolicy),
    E("AAudioStreamBuilder_setChannelMask", b_setChannelMask),
    E("AAudioStreamBuilder_setDataCallback", b_setDataCallback),
    E("AAudioStreamBuilder_setErrorCallback", b_setErrorCallback),
    E("AAudioStreamBuilder_openStream", b_openStream),
    E("AAudioStreamBuilder_delete", b_delete),
    E("AAudioStream_requestStart", s_requestStart),
    E("AAudioStream_requestPause", s_requestPause),
    E("AAudioStream_requestFlush", s_requestFlush),
    E("AAudioStream_requestStop", s_requestStop),
    E("AAudioStream_close", s_close),
    E("AAudioStream_getState", s_getState),
    E("AAudioStream_waitForStateChange", s_waitForStateChange),
    E("AAudioStream_write", s_write),
    E("AAudioStream_getSampleRate", s_getSampleRate),
    E("AAudioStream_getChannelCount", s_getChannelCount),
    E("AAudioStream_getSamplesPerFrame", s_getSamplesPerFrame),
    E("AAudioStream_getFormat", s_getFormat),
    E("AAudioStream_getDirection", s_getDirection),
    E("AAudioStream_getDeviceId", s_getDeviceId),
    E("AAudioStream_getSharingMode", s_getSharingMode),
    E("AAudioStream_getPerformanceMode", s_getPerformanceMode),
    E("AAudioStream_getFramesPerBurst", s_getFramesPerBurst),
    E("AAudioStream_getFramesPerDataCallback", s_getFramesPerDataCallback),
    E("AAudioStream_getBufferSizeInFrames", s_getBufferSizeInFrames),
    E("AAudioStream_setBufferSizeInFrames", s_setBufferSizeInFrames),
    E("AAudioStream_getBufferCapacityInFrames", s_getBufferCapacityInFrames),
    E("AAudioStream_getXRunCount", s_getXRunCount),
    E("AAudioStream_isMMapUsed", s_isMMapUsed),
    E("AAudioStream_getFramesWritten", s_getFramesWritten),
    E("AAudioStream_getFramesRead", s_getFramesRead),
    E("AAudioStream_getUsage", s_getUsage),
    E("AAudioStream_getContentType", s_getContentType),
    E("AAudioStream_getSessionId", s_getSessionId),
    E("AAudioStream_getTimestamp", s_getTimestamp),
};

void *na_aaudio_lookup(const char *name) {
    if (!name || strncmp(name, "AAudio", 6) != 0) return NULL;
    for (unsigned i = 0; i < sizeof(aaudio_table) / sizeof(aaudio_table[0]); i++)
        if (strcmp(aaudio_table[i].name, name) == 0) return aaudio_table[i].fn;
    l_warn("[AAudio] %s is not implemented", name);
    return NULL;
}
