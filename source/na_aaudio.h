/*
 * Copyright (C) 2026 Vita port contributors
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

/**
 * @file  na_aaudio.h
 * @brief AAudio output on SceAudio (dlopen("libaaudio.so") + dlsym).
 */

#ifndef NA_AAUDIO_H
#define NA_AAUDIO_H

/** AAudio function by name, or NULL. */
void *na_aaudio_lookup(const char *name);

#endif // NA_AAUDIO_H
