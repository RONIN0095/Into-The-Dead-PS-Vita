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
 * - ASensor: an accelerometer driven by the left stick (see below).
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

static int sensor_poll(na_looper *l, int timeout_ms, int *fd, int *events, void **data, int *wait_ms);

int ALooper_pollOnce_na(int timeout, int *fd, int *events, void **data) {
    na_looper *l = thread_looper;
    if (!l) l = ALooper_prepare_na(0);
    if (l->woken) {
        l->woken = 0;
        return ALOOPER_POLL_WAKE;
    }
    // the accelerometer's events come through the looper they were set up on
    int wait_ms = timeout;
    int ident = sensor_poll(l, timeout, fd, events, data, &wait_ms);
    if (ident != ALOOPER_POLL_TIMEOUT) return ident;
    if (timeout == 0) return ALOOPER_POLL_TIMEOUT;
    timeout = wait_ms;
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
 * ASensor: one accelerometer, driven by the left stick (and the D-pad).
 * Into the Dead steers by tilting the phone by default; the stick tilts a
 * virtual phone left and right (up to TILT_MAX degrees). Unity reads the
 * sensor through an event queue on a looper: pollOnce on that looper
 * reports the queue's ident (or calls its callback) at the queue's event
 * rate, and getEvents hands out the current tilt.
 *
 * Android's accelerometer measures the reaction to gravity in the device's
 * frame: lying flat it reads z = +9.81; with the right edge lowered x goes
 * negative. The Vita's natural orientation is landscape (rotation 0), so
 * the device's x is the screen's x.
 */
#include <math.h>
#include <psp2/ctrl.h>

#define SENSOR_TYPE_ACCELEROMETER 1
#define TILT_MAX 35.0f
#define GRAVITY 9.80665f

typedef struct {
    int32_t version, sensor, type, reserved0;
    int64_t timestamp;
    float data[16];
    uint32_t flags;
    int32_t reserved1[3];
} na_sensor_event; // ASensorEvent, 104 bytes

static int sensor_manager;
static int accel_sensor = 1; // its address is the sensor handle Unity keeps
static struct {
    na_looper *looper;
    int ident;
    int (*cb)(int fd, int events, void *data);
    void *data;
    volatile int enabled;
    int rate_us;
    uint64_t next_us;
} sensor_queue;

void *ASensorManager_getInstance_na(void) { return &sensor_manager; }

int ASensorManager_getSensorList_na(void *m, void ***list) {
    static void *sensors[2] = { &accel_sensor, NULL };
    if (list) *list = sensors;
    return 1;
}

void *ASensorManager_getDefaultSensor_na(void *m, int type) {
    return type == SENSOR_TYPE_ACCELEROMETER ? &accel_sensor : NULL;
}

void *ASensorManager_createEventQueue_na(void *m, void *looper, int ident, void *cb, void *data) {
    sensor_queue.looper = looper;
    sensor_queue.ident = ident;
    sensor_queue.cb = cb;
    sensor_queue.data = data;
    if (!sensor_queue.rate_us) sensor_queue.rate_us = 20000;
    l_info("[sensor] event queue on looper %p, ident %d, callback %p", looper, ident, cb);
    return &sensor_queue;
}

int ASensorManager_destroyEventQueue_na(void *m, void *q) {
    sensor_queue.enabled = 0;
    sensor_queue.looper = NULL;
    return 0;
}

int ASensorEventQueue_enableSensor_na(void *q, void *s) {
    if (s != &accel_sensor) return -1;
    sensor_queue.enabled = 1;
    l_info("[sensor] accelerometer on (left stick)");
    return 0;
}

int ASensorEventQueue_disableSensor_na(void *q, void *s) {
    if (s == &accel_sensor) sensor_queue.enabled = 0;
    return 0;
}

int ASensorEventQueue_setEventRate_na(void *q, void *s, int32_t us) {
    if (s != &accel_sensor) return -1;
    sensor_queue.rate_us = us < 10000 ? 10000 : us;
    return 0;
}

static int sensor_due(void) {
    return sensor_queue.enabled && sceKernelGetProcessTimeWide() >= sensor_queue.next_us;
}

int ASensorEventQueue_hasEvents_na(void *q) { return sensor_due(); }

// Left stick x (or the D-pad) as -1..1
static float stick_tilt(void) {
    SceCtrlData pad;
    memset(&pad, 0, sizeof(pad));
    if (sceCtrlPeekBufferPositive(0, &pad, 1) < 0) return 0.0f;
    float x = ((int)pad.lx - 128) / 127.0f;
    if (x > -0.15f && x < 0.15f) x = 0.0f; // dead zone
    if (pad.buttons & SCE_CTRL_LEFT) x = -1.0f;
    if (pad.buttons & SCE_CTRL_RIGHT) x = 1.0f;
    if (x > 1.0f) x = 1.0f;
    if (x < -1.0f) x = -1.0f;
    return x;
}

int ASensorEventQueue_getEvents_na(void *q, void *events, int count) {
    if (!sensor_queue.enabled || count < 1 || !events || !sensor_due()) return 0;
    uint64_t now = sceKernelGetProcessTimeWide();
    sensor_queue.next_us = now + sensor_queue.rate_us;
    float a = stick_tilt() * TILT_MAX * 3.14159265f / 180.0f;
    na_sensor_event *e = events;
    memset(e, 0, sizeof(*e));
    e->version = sizeof(*e);
    e->sensor = 1;
    e->type = SENSOR_TYPE_ACCELEROMETER;
    e->timestamp = (int64_t)now * 1000;
    e->data[0] = -GRAVITY * sinf(a); // right edge down: x negative
    e->data[1] = 0.0f;
    e->data[2] = GRAVITY * cosf(a);
    e->data[3] = 3.0f; // accuracy: high
    return 1;
}

// From ALooper_pollOnce: the queue's ident (or its callback) when an event is due
static int sensor_poll(na_looper *l, int timeout_ms, int *fd, int *events, void **data, int *wait_ms) {
    if (!sensor_queue.enabled || (na_looper *)sensor_queue.looper != l) return ALOOPER_POLL_TIMEOUT;
    if (sensor_due()) {
        if (sensor_queue.cb) {
            sensor_queue.cb(-1, 1, sensor_queue.data);
            return -2; // ALOOPER_POLL_CALLBACK
        }
        if (fd) *fd = -1;
        if (events) *events = 1;
        if (data) *data = sensor_queue.data;
        return sensor_queue.ident;
    }
    // wait no longer than until the next event
    uint64_t now = sceKernelGetProcessTimeWide();
    int left = (int)((sensor_queue.next_us - now) / 1000) + 1;
    if (timeout_ms < 0 || left < timeout_ms) *wait_ms = left;
    return ALOOPER_POLL_TIMEOUT;
}

const char *ASensor_getName_na(void *s) { return "Vita left stick"; }
const char *ASensor_getVendor_na(void *s) { return "Sony"; }
int ASensor_getType_na(void *s) { return s == &accel_sensor ? SENSOR_TYPE_ACCELEROMETER : -1; }
float ASensor_getResolution_na(void *s) { return 0.01f; }
int ASensor_getMinDelay_na(void *s) { return 10000; }

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
