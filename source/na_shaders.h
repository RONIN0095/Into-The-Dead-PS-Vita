/*
 * Copyright (C) 2026 Vita port contributors
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

/**
 * @file  na_shaders.h
 * @brief GLSL ES 3.00 (Unity/hlslcc) to GLSL ES 1.00 (vitaGL).
 */

#ifndef NA_SHADERS_H
#define NA_SHADERS_H

/**
 * Translates one shader stage. Returns a malloc'd string, or NULL if the
 * source is not "#version 300 es" (then it is passed through unchanged).
 */
char *na_shaders_translate(const char *src, int is_fragment, int *changed);

#endif // NA_SHADERS_H
