/*
 * Copyright (C) 2026 Vita port contributors
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

/**
 * @file  na_android.h
 * @brief The NDK parts libunity.so imports: ALooper, ANativeWindow, ASensor,
 *        AChoreographer.
 */

#ifndef NA_ANDROID_H
#define NA_ANDROID_H

#include <stdint.h>

#define SCREEN_W 960
#define SCREEN_H 544

/** The window Unity renders into (what ANativeWindow_fromSurface returns). */
void *na_native_window(void);

/** Runs the frame callbacks Unity posted with AChoreographer (call once per frame). */
void na_choreographer_tick(void);

void *ALooper_forThread_na(void);
void *ALooper_prepare_na(int opts);
void ALooper_acquire_na(void *l);
void ALooper_release_na(void *l);
int ALooper_pollOnce_na(int timeout, int *fd, int *events, void **data);
void ALooper_wake_na(void *l);
int ALooper_addFd_na(void *l, int fd, int ident, int events, void *cb, void *data);
int ALooper_removeFd_na(void *l, int fd);

void *ANativeWindow_fromSurface_na(void *env, void *surface);
void ANativeWindow_acquire_na(void *w);
void ANativeWindow_release_na(void *w);
int32_t ANativeWindow_getWidth_na(void *w);
int32_t ANativeWindow_getHeight_na(void *w);
int32_t ANativeWindow_setBuffersGeometry_na(void *w, int32_t width, int32_t height, int32_t format);

void *ASensorManager_getInstance_na(void);
int ASensorManager_getSensorList_na(void *m, void ***list);
void *ASensorManager_getDefaultSensor_na(void *m, int type);
void *ASensorManager_createEventQueue_na(void *m, void *looper, int ident, void *cb, void *data);
int ASensorManager_destroyEventQueue_na(void *m, void *q);
int ASensorEventQueue_enableSensor_na(void *q, void *s);
int ASensorEventQueue_disableSensor_na(void *q, void *s);
int ASensorEventQueue_setEventRate_na(void *q, void *s, int32_t us);
int ASensorEventQueue_hasEvents_na(void *q);
int ASensorEventQueue_getEvents_na(void *q, void *events, int count);
const char *ASensor_getName_na(void *s);
const char *ASensor_getVendor_na(void *s);
int ASensor_getType_na(void *s);
float ASensor_getResolution_na(void *s);
int ASensor_getMinDelay_na(void *s);

void *AChoreographer_getInstance_na(void);
void AChoreographer_postFrameCallback_na(void *c, void (*cb)(long, void *), void *data);
void AChoreographer_postFrameCallback64_na(void *c, void (*cb)(int64_t, void *), void *data);

#define NA_ANDROID_DYNLIB_ENTRIES \
    { "ALooper_forThread", (uintptr_t)&ALooper_forThread_na }, \
    { "ALooper_prepare", (uintptr_t)&ALooper_prepare_na }, \
    { "ALooper_acquire", (uintptr_t)&ALooper_acquire_na }, \
    { "ALooper_release", (uintptr_t)&ALooper_release_na }, \
    { "ALooper_pollOnce", (uintptr_t)&ALooper_pollOnce_na }, \
    { "ALooper_wake", (uintptr_t)&ALooper_wake_na }, \
    { "ALooper_addFd", (uintptr_t)&ALooper_addFd_na }, \
    { "ALooper_removeFd", (uintptr_t)&ALooper_removeFd_na }, \
    { "ANativeWindow_fromSurface", (uintptr_t)&ANativeWindow_fromSurface_na }, \
    { "ANativeWindow_acquire", (uintptr_t)&ANativeWindow_acquire_na }, \
    { "ANativeWindow_release", (uintptr_t)&ANativeWindow_release_na }, \
    { "ANativeWindow_getWidth", (uintptr_t)&ANativeWindow_getWidth_na }, \
    { "ANativeWindow_getHeight", (uintptr_t)&ANativeWindow_getHeight_na }, \
    { "ANativeWindow_setBuffersGeometry", (uintptr_t)&ANativeWindow_setBuffersGeometry_na }, \
    { "ASensorManager_getInstance", (uintptr_t)&ASensorManager_getInstance_na }, \
    { "ASensorManager_getSensorList", (uintptr_t)&ASensorManager_getSensorList_na }, \
    { "ASensorManager_getDefaultSensor", (uintptr_t)&ASensorManager_getDefaultSensor_na }, \
    { "ASensorManager_createEventQueue", (uintptr_t)&ASensorManager_createEventQueue_na }, \
    { "ASensorManager_destroyEventQueue", (uintptr_t)&ASensorManager_destroyEventQueue_na }, \
    { "ASensorEventQueue_enableSensor", (uintptr_t)&ASensorEventQueue_enableSensor_na }, \
    { "ASensorEventQueue_disableSensor", (uintptr_t)&ASensorEventQueue_disableSensor_na }, \
    { "ASensorEventQueue_setEventRate", (uintptr_t)&ASensorEventQueue_setEventRate_na }, \
    { "ASensorEventQueue_hasEvents", (uintptr_t)&ASensorEventQueue_hasEvents_na }, \
    { "ASensorEventQueue_getEvents", (uintptr_t)&ASensorEventQueue_getEvents_na }, \
    { "ASensor_getName", (uintptr_t)&ASensor_getName_na }, \
    { "ASensor_getVendor", (uintptr_t)&ASensor_getVendor_na }, \
    { "ASensor_getType", (uintptr_t)&ASensor_getType_na }, \
    { "ASensor_getResolution", (uintptr_t)&ASensor_getResolution_na }, \
    { "ASensor_getMinDelay", (uintptr_t)&ASensor_getMinDelay_na }, \
    { "AChoreographer_getInstance", (uintptr_t)&AChoreographer_getInstance_na }, \
    { "AChoreographer_postFrameCallback", (uintptr_t)&AChoreographer_postFrameCallback_na }, \
    { "AChoreographer_postFrameCallback64", (uintptr_t)&AChoreographer_postFrameCallback64_na },

#endif // NA_ANDROID_H
