/*
 * Copyright (C) 2026 Vita port contributors
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

/**
 * @file  na_input.h
 * @brief Touch and buttons as Android input events.
 */

#ifndef NA_INPUT_H
#define NA_INPUT_H

#include <falso_jni/FalsoJNI.h>

/** Reads touch and buttons and injects the changes into Unity. */
void na_input_poll(void);

/** Answers MotionEvent/KeyEvent getters for our event objects. Returns 1 if handled. */
int na_input_event_call(const char *name, const char *sig, jobject self, const jvalue *args, int argc, jvalue *r);

/** MotionEvent.obtain(event) and similar static copies. Returns 1 if handled. */
int na_input_static_call(const char *cls, const char *name, const jvalue *args, int argc, jvalue *r);

// Draws the menu cursor (na_egl.c, before the swap)
void na_input_draw_cursor(void);

#endif // NA_INPUT_H