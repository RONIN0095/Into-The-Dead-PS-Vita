/*
 * Copyright (C) 2022-2024 Volodymyr Atamanenko
 * Copyright (C) 2026      Vita port contributors
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

/**
 * @file  glutil.h
 * @brief vitaGL setup.
 */

#ifndef SOLOADER_GLUTIL_H
#define SOLOADER_GLUTIL_H

#include <vitaGL.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Checks for libshacccg.suprx and sets up vitaGL options that must come before init. */
void gl_preload();

/** Initializes vitaGL (960x544, no MSAA). */
void gl_init();

void gl_swap();

#ifdef __cplusplus
};
#endif

#endif // SOLOADER_GLUTIL_H
