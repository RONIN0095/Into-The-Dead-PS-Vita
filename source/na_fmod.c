/*
 * Copyright (C) 2026 Vita port contributors
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

/**
 * @file  na_fmod.c
 * @brief org.fmod.FMODAudioDevice: the game's sound output.
 *
 * Unity 2020.3's FMOD does not open OpenSL ES or AAudio on Android: its Java
 * class FMODAudioDevice runs a thread that asks the mixer for sound
 * (fmodProcess fills a direct ByteBuffer with 16-bit PCM) and writes it to
 * an AudioTrack. Without that class the game was silent. This is the class:
 * start() starts a thread that waits for the mixer, opens a Vita audio port
 * with the mixer's rate and feeds it block by block (sceAudioOutOutput
 * blocks for a block's time, which paces the loop as AudioTrack.write does).
 *
 * fmodGetInfo ids (libunity.so): 0 sample rate, 1 DSP buffer length in
 * frames, 2 number of DSP buffers, 3 mixer running, 4 channels.
 */

#include <malloc.h>
#include <stdint.h>
#include <string.h>

#include <psp2/audioout.h>
#include <psp2/kernel/threadmgr.h>

#include "jni_objects.h"
#include "na_threads.h"
#include "utils/logger.h"

void *na_unity_native(const char *name, const char *args); // na_unity.c

static volatile int running = 0, quit = 0;
static jobject device;
static SceUID thread = -1;

static int valid_rate(int r) {
    return r == 8000 || r == 11025 || r == 12000 || r == 16000 || r == 22050 || r == 24000 || r == 32000 || r == 44100 ||
           r == 48000;
}

static int fmod_thread(SceSize args, void *argp) {
    na_thread_register("fmod", (void *)fmod_thread);
    JNIEnv *env = jo_env();
    int (*info)(JNIEnv *, jobject, jint) = na_unity_native("fmodGetInfo", "(I");
    int (*process)(JNIEnv *, jobject, jobject) = na_unity_native("fmodProcess", "(Ljava/nio/ByteBuffer;");
    if (!info || !process) {
        l_error("[FMOD] fmodGetInfo/fmodProcess are not registered: no sound");
        return 0;
    }
    // The mixer exists once Unity's audio has started
    int waited = 0;
    while (!quit && !(info(env, device, 3) == 1 && info(env, device, 0) > 0)) {
        sceKernelDelayThread(20000);
        if (++waited == 1500) l_warn("[FMOD] the mixer has not started after 30 s");
    }
    if (quit) return 0;
    int rate = info(env, device, 0), len = info(env, device, 1), nbuf = info(env, device, 2), ch = info(env, device, 4);
    l_info("[FMOD] mixer: %d Hz, %d channels, DSP buffer %d frames x %d", rate, ch, len, nbuf);
    if (!valid_rate(rate) || len < 64 || len > 16384 || ch < 1 || ch > 2) {
        l_error("[FMOD] the Vita has no audio port for that: no sound");
        return 0;
    }
    // Audio port blocks are multiples of 64 frames; FMOD's buffers are powers of two
    int grain = (len + 63) & ~63;
    int port = sceAudioOutOpenPort(SCE_AUDIO_OUT_PORT_TYPE_BGM, grain, rate,
                                   ch == 1 ? SCE_AUDIO_OUT_MODE_MONO : SCE_AUDIO_OUT_MODE_STEREO);
    if (port < 0) {
        l_error("[FMOD] sceAudioOutOpenPort(%d frames, %d Hz): 0x%x", grain, rate, port);
        return 0;
    }
    int vol[2] = { SCE_AUDIO_VOLUME_0DB, SCE_AUDIO_VOLUME_0DB };
    sceAudioOutSetVolume(port, SCE_AUDIO_VOLUME_FLAG_L_CH | SCE_AUDIO_VOLUME_FLAG_R_CH, vol);
    size_t bytes = (size_t)len * ch * 2;
    int16_t *pcm = memalign(64, bytes), *out = memalign(64, (size_t)grain * ch * 2);
    jobject buf = (*env)->NewDirectByteBuffer(env, pcm, (jlong)bytes);
    if (!pcm || !out || !buf) {
        l_error("[FMOD] no memory for the sound buffers");
        return 0;
    }
    memset(out, 0, (size_t)grain * ch * 2);
    int filled = 0; // frames in `out`
    running = 1;
    l_info("[FMOD] sound running (blocks of %d frames)", grain);
    while (!quit) {
        memset(pcm, 0, bytes);
        process(env, device, buf);
        int done = 0;
        while (done < len) {
            int n = grain - filled;
            if (n > len - done) n = len - done;
            memcpy(out + (size_t)filled * ch, pcm + (size_t)done * ch, (size_t)n * ch * 2);
            filled += n;
            done += n;
            if (filled == grain) {
                sceAudioOutOutput(port, out);
                filled = 0;
            }
        }
    }
    running = 0;
    sceAudioOutReleasePort(port);
    return 0;
}

/** Calls on org/fmod/FMODAudioDevice (from na_java.c). Returns 1 if handled. */
int na_fmod_call(const char *name, jobject self, jvalue *r) {
    if (!strcmp(name, "start")) {
        if (thread < 0) {
            device = self;
            quit = 0;
            // above the game's threads: sound must not wait for a slow frame
            thread = sceKernelCreateThread("fmod", fmod_thread, 0x10000100 - 12, 0x40000, 0, 0, NULL);
            if (thread >= 0) sceKernelStartThread(thread, 0, NULL);
            l_info("[FMOD] FMODAudioDevice.start(): thread 0x%x", (unsigned)thread);
        }
        return 1;
    }
    if (!strcmp(name, "stop")) {
        // the game pauses its sound when it loses focus; the thread keeps running here
        return 1;
    }
    if (!strcmp(name, "isRunning")) {
        r->z = thread >= 0;
        return 1;
    }
    if (!strcmp(name, "close")) return 1;
    return 0;
}
