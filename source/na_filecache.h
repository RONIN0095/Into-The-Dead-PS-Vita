/*
 * Copyright (C) 2026 Vita port contributors
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

#ifndef NA_FILECACHE_H
#define NA_FILECACHE_H

#include <stddef.h>
#include <stdint.h>

/** A read of an asset file: 1 and the result in *out when it came from memory. */
int na_filecache_read(int fd, void *buf, size_t n, long *out);

/** The game read the card itself just now (reading ahead waits). */
void na_filecache_game_read(void);

/** An asset file was written again: forget all copies. */
void na_filecache_drop(void);

/** Main loop, every frame: starts the read-ahead thread, gives memory back when the heap is tight. */
void na_filecache_tick(int frame);

/** Size of the cache in KB; *hits_kb gets what was read from it so far. */
uint32_t na_filecache_kb(uint32_t *hits_kb);

#endif
