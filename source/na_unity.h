/*
 * Copyright (C) 2026 Vita port contributors
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

/**
 * @file  na_unity.h
 * @brief Starting the Unity player and running its frames.
 */

#ifndef NA_UNITY_H
#define NA_UNITY_H

#include <falso_jni/FalsoJNI.h>

/** JNI_OnLoad of libunity.so, initJni, surface, resume, focus. */
void na_unity_start(void);

/** One nativeRender() (Unity swaps buffers itself). Returns its result. */
int na_unity_frame(void);

jobject na_unity_player(void);

/** The Activity Unity was started with (UnityPlayer.currentActivity). */
jobject na_unity_activity(void);

/** UnityPlayer.nativeInjectEvent(event, 0). */
void na_unity_inject_event(jobject ev);

#endif // NA_UNITY_H
