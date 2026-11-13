/*
 * Copyright (C) 2026 Vita port contributors
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

/**
 * @file  na_android.c
 * @brief ALooper, ANativeWindow, ASensor and AChoreographer for libunity.so.
 *
 * - ALooper: one looper per thread with a set of fds and a wake flag.
 *   pollOnce() sleeps until woken or the timeout passes; fds are never
 *   reported ready (Unity only registers its own pipes, nothing writes them
 *   behind its back).
 * - ANativeWindow: a single 960x544 window, the Vita screen.
 * - ASensor: no sensors.
 * - AChoreographer: frame callbacks are called from the main loop before
 *   each nativeRender().
 */

#include "na_android.h"

#include <stdlib.h>
#include <string.h>

#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>

#include "utils/logger.h"

#define ALOOPER_POLL_WAKE (-1)
#define ALOOPER_POLL_TIMEOUT (-3)

/*
 * ALooper
 */
typedef struct {
    int refs;
    SceUID sema;
    volatile int woken;
} na_looper;

static __thread na_looper *thread_looper = NULL;

// Android's main/UI thread always has a looper; ours get one on first use.
void *ALooper_forThread_na(void) {
    if (!thread_looper) ALooper_prepare_na(0);
    return thread_looper;
}

void *ALooper_prepare_na(int opts);
void *ALooper_prepare_na(int opts) {
    if (!thread_looper) {
        na_looper *l = calloc(1, sizeof(*l));
        l->refs = 1;
        l->sema = sceKernelCreateSema("ALooper", 0, 0, 1, NULL);
        thread_looper = l;
        l_debug("ALooper_prepare: new looper %p for thread 0x%x", l, sceKernelGetThreadId());
    }
    return thread_looper;
}

void ALooper_acquire_na(void *l) {
    if (l) ((na_looper *)l)->refs++;
}

void ALooper_release_na(void *l) {
    if (l) ((na_looper *)l)->refs--;
}

int ALooper_pollOnce_na(int timeout, int *fd, int *events, void **data) {
    na_looper *l = thread_looper;
    if (!l) l = ALooper_prepare_na(0);
    if (l->woken) {
        l->woken = 0;
        return ALOOPER_POLL_WAKE;
    }
    if (timeout == 0) return ALOOPER_POLL_TIMEOUT;
    SceUInt t = timeout < 0 ? 0 : (SceUInt)timeout * 1000;
    int r = sceKernelWaitSema(l->sema, 1, timeout < 0 ? NULL : &t);
    if (r == 0) {
        l->woken = 0;
        return ALOOPER_POLL_WAKE;
    }
    return ALOOPER_POLL_TIMEOUT;
}

void ALooper_wake_na(void *p) {
    na_looper *l = p;
    if (!l) return;
    l->woken = 1;
    sceKernelSignalSema(l->sema, 1);
}

int ALooper_addFd_na(void *l, int fd, int ident, int events, void *cb, void *data) {
    l_debug("ALooper_addFd(%p, fd %d, ident %d, events 0x%x)", l, fd, ident, events);
    return 1;
}

int ALooper_removeFd_na(void *l, int fd) {
    return 1;
}

/*
 * ANativeWindow
 */
static struct {
    uint32_t magic;
    int refs;
} the_window = { 0x5f776e64, 1 };

void *na_native_window(void) {
    return &the_window;
}

void *ANativeWindow_fromSurface_na(void *env, void *surface) {
    l_debug("ANativeWindow_fromSurface(%p)", surface);
    return &the_window;
}

void ANativeWindow_acquire_na(void *w) {}
void ANativeWindow_release_na(void *w) {}
int32_t ANativeWindow_getWidth_na(void *w) { return SCREEN_W; }
int32_t ANativeWindow_getHeight_na(void *w) { return SCREEN_H; }

int32_t ANativeWindow_setBuffersGeometry_na(void *w, int32_t width, int32_t height, int32_t format) {
    l_debug("ANativeWindow_setBuffersGeometry(%d, %d, %d)", width, height, format);
    return 0;
}

/*
 * ASensor: none
 */
static int sensor_manager;
static int sensor_queue;

void *ASensorManager_getInstance_na(void) { return &sensor_manager; }

int ASensorManager_getSensorList_na(void *m, void ***list) {
    static void *empty[1] = { NULL };
    if (list) *list = empty;
    return 0;
}

void *ASensorManager_getDefaultSensor_na(void *m, int type) { return NULL; }
void *ASensorManager_createEventQueue_na(void *m, void *looper, int ident, void *cb, void *data) { return &sensor_queue; }
int ASensorManager_destroyEventQueue_na(void *m, void *q) { return 0; }
int ASensorEventQueue_enableSensor_na(void *q, void *s) { return -1; }
int ASensorEventQueue_disableSensor_na(void *q, void *s) { return 0; }
int ASensorEventQueue_setEventRate_na(void *q, void *s, int32_t us) { return -1; }
int ASensorEventQueue_hasEvents_na(void *q) { return 0; }
int ASensorEventQueue_getEvents_na(void *q, void *events, int count) { return 0; }
const char *ASensor_getName_na(void *s) { return "none"; }
const char *ASensor_getVendor_na(void *s) { return "none"; }
int ASensor_getType_na(void *s) { return -1; }
float ASensor_getResolution_na(void *s) { return 0.0f; }
int ASensor_getMinDelay_na(void *s) { return 0; }

/*
 * AChoreographer
 */
#define MAX_FRAME_CBS 16

typedef struct {
    void *cb;
    void *data;
    int is64;
} frame_cb;

static int choreographer;
static frame_cb pending[MAX_FRAME_CBS];
static int n_pending = 0;
static SceKernelLwMutexWork cb_lock;
static int cb_lock_ready = 0;

static void ensure_lock(void) {
    if (!cb_lock_ready) {
        sceKernelCreateLwMutex(&cb_lock, "choreographer", 0, 0, NULL);
        cb_lock_ready = 1;
    }
}

void *AChoreographer_getInstance_na(void) {
    ensure_lock();
    return &choreographer;
}

static void post(void *cb, void *data, int is64) {
    ensure_lock();
    sceKernelLockLwMutex(&cb_lock, 1, NULL);
    if (n_pending < MAX_FRAME_CBS)
        pending[n_pending++] = (frame_cb){ cb, data, is64 };
    else
        l_warn("AChoreographer: too many pending callbacks");
    sceKernelUnlockLwMutex(&cb_lock, 1);
}

void AChoreographer_postFrameCallback_na(void *c, void (*cb)(long, void *), void *data) {
    post((void *)cb, data, 0);
}

void AChoreographer_postFrameCallback64_na(void *c, void (*cb)(int64_t, void *), void *data) {
    post((void *)cb, data, 1);
}

void na_choreographer_tick(void) {
    if (!cb_lock_ready) return;
    frame_cb now[MAX_FRAME_CBS];
    sceKernelLockLwMutex(&cb_lock, 1, NULL);
    int n = n_pending;
    memcpy(now, pending, sizeof(frame_cb) * n);
    n_pending = 0;
    sceKernelUnlockLwMutex(&cb_lock, 1);

    int64_t ns = (int64_t)sceKernelGetProcessTimeWide() * 1000;
    for (int i = 0; i < n; i++) {
        if (now[i].is64)
            ((void (*)(int64_t, void *))now[i].cb)(ns, now[i].data);
        else
            ((void (*)(long, void *))now[i].cb)((long)ns, now[i].data);
    }
}
