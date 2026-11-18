/*
 * Copyright (C) 2026 Vita port contributors
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

/**
 * @file  na_apkcache.c
 * @brief base.apk through a block cache.
 *
 * Unity reads its asset files out of the APK with fopen/fseek/fread, in
 * many small pieces with a seek before each. On newlib every seek throws
 * away the stream's 1 KB buffer, so each piece became a seek and a small
 * read on the memory card, and a load spent most of its time there (the
 * main thread was in fseek/fread whenever the watchdog looked; the menu took
 * 46 s, starting a run 70 s).
 *
 * Here base.apk is opened as a funopen() stream (unbuffered in newlib) whose
 * reads come from a cache of 64 KB blocks shared by all its handles. A miss
 * reads the whole block in one go; a big read goes straight into the
 * caller's buffer. The handle's real file descriptor is kept in the FILE so
 * fileno() still works.
 */

#include <errno.h>
#include <fcntl.h>
#include <malloc.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <psp2/io/fcntl.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/sysmem.h>
#include <psp2/kernel/threadmgr.h>

#include "utils/logger.h"

#define BLOCK (64 * 1024)
#define DIRECT_MIN (256 * 1024) // reads this big skip the cache (their middle part)

typedef struct {
    int64_t block; // block index, -1 = empty
    uint32_t used; // last use
} slot;

static uint8_t *cache_mem;
static slot *slots;
static int n_slots;
static uint32_t clock_tick;
static SceKernelLwMutexWork lock_work;
static int ready = 0;
static SceUID cache_fd = -1; // one descriptor for all block reads (sceIoPread)
static int64_t apk_size;

// stats for the log
static uint32_t st_reads, st_hits, st_misses, st_direct_kb, st_io_us;

static void cache_init(void) {
    if (ready) return;
    sceKernelCreateLwMutex(&lock_work, "apkcache", 0, 0, NULL);
    // As big as memory allows: 12 MB, else less
    static const int sizes_mb[] = { 12, 8, 4, 2 };
    for (unsigned i = 0; i < sizeof(sizes_mb) / sizeof(sizes_mb[0]) && !cache_mem; i++) {
        size_t sz = (size_t)sizes_mb[i] << 20;
        SceUID b = sceKernelAllocMemBlock("apkcache", SCE_KERNEL_MEMBLOCK_TYPE_USER_RW, sz, NULL);
        if (b >= 0) {
            void *p = NULL;
            sceKernelGetMemBlockBase(b, &p);
            cache_mem = p;
            n_slots = (int)(sz / BLOCK);
        }
    }
    if (!cache_mem) {
        cache_mem = memalign(64, 1 << 20);
        n_slots = cache_mem ? (1 << 20) / BLOCK : 0;
    }
    slots = calloc(n_slots ? n_slots : 1, sizeof(slot));
    for (int i = 0; i < n_slots; i++) slots[i].block = -1;
    ready = 1;
    l_info("[apkcache] %d KB of 64 KB blocks for base.apk", n_slots * BLOCK / 1024);
}

static int read_at(void *dst, size_t n, int64_t off) {
    uint64_t t0 = sceKernelGetProcessTimeWide();
    size_t got = 0;
    while (got < n) {
        int r = sceIoPread(cache_fd, (char *)dst + got, n - got, off + got);
        if (r <= 0) break;
        got += r;
    }
    st_io_us += (uint32_t)(sceKernelGetProcessTimeWide() - t0);
    return (int)got;
}

// The block's data (at most BLOCK bytes, fewer at the end); *len its length
static uint8_t *get_block(int64_t blk, int *len) {
    int victim = 0;
    uint32_t oldest = 0xffffffff;
    for (int i = 0; i < n_slots; i++) {
        if (slots[i].block == blk) {
            slots[i].used = ++clock_tick;
            st_hits++;
            int64_t left = apk_size - blk * BLOCK;
            *len = left < BLOCK ? (int)left : BLOCK;
            return cache_mem + (size_t)i * BLOCK;
        }
        if (slots[i].used < oldest) {
            oldest = slots[i].used;
            victim = i;
        }
    }
    st_misses++;
    uint8_t *p = cache_mem + (size_t)victim * BLOCK;
    int r = read_at(p, BLOCK, blk * BLOCK);
    if (r <= 0) {
        slots[victim].block = -1;
        *len = 0;
        return NULL;
    }
    slots[victim].block = blk;
    slots[victim].used = ++clock_tick;
    *len = r;
    return p;
}

typedef struct {
    int fd;      // newlib descriptor (for fileno)
    int64_t pos;
} handle;

static int h_read(void *cookie, char *buf, int n) {
    handle *h = cookie;
    if (n <= 0 || h->pos >= apk_size) return 0;
    if (h->pos + n > apk_size) n = (int)(apk_size - h->pos);
    sceKernelLockLwMutex(&lock_work, 1, NULL);
    st_reads++;
    int done = 0;
    while (done < n) {
        int64_t at = h->pos + done;
        int in_blk = (int)(at % BLOCK);
        // big reads: whole blocks straight into the buffer
        if (in_blk == 0 && n - done >= DIRECT_MIN) {
            int whole = ((n - done) / BLOCK) * BLOCK;
            int r = read_at(buf + done, whole, at);
            st_direct_kb += r > 0 ? r / 1024 : 0;
            if (r <= 0) break;
            done += r;
            continue;
        }
        int len;
        uint8_t *b = n_slots ? get_block(at / BLOCK, &len) : NULL;
        if (!b) {
            int r = read_at(buf + done, n - done, at); // no cache
            if (r > 0) done += r;
            break;
        }
        int take = len - in_blk;
        if (take <= 0) break;
        if (take > n - done) take = n - done;
        memcpy(buf + done, b + in_blk, take);
        done += take;
    }
    sceKernelUnlockLwMutex(&lock_work, 1);
    h->pos += done;
    return done;
}

static fpos_t h_seek(void *cookie, fpos_t off, int whence) {
    handle *h = cookie;
    int64_t np = whence == SEEK_SET ? (int64_t)off : whence == SEEK_CUR ? h->pos + off : apk_size + off;
    if (np < 0) {
        errno = EINVAL;
        return -1;
    }
    h->pos = np;
    return (fpos_t)np;
}

static int h_close(void *cookie) {
    handle *h = cookie;
    if (h->fd >= 0) close(h->fd);
    free(h);
    return 0;
}

/** fopen for base.apk (read only); NULL if it can't be done this way. */
FILE *na_apkcache_open(const char *path) {
    cache_init();
    if (cache_fd < 0) {
        cache_fd = sceIoOpen(path, SCE_O_RDONLY, 0);
        if (cache_fd < 0) return NULL;
        apk_size = sceIoLseek(cache_fd, 0, SCE_SEEK_END);
    }
    handle *h = calloc(1, sizeof(handle));
    if (!h) return NULL;
    h->fd = open(path, O_RDONLY);
    FILE *f = funopen(h, h_read, NULL, h_seek, h_close);
    if (!f) {
        if (h->fd >= 0) close(h->fd);
        free(h);
        return NULL;
    }
    setvbuf(f, NULL, _IONBF, 0);
    if (h->fd >= 0) f->_file = (short)h->fd;
    return f;
}

/** One log line with what the cache did since the last one. */
void na_apkcache_report(void) {
    if (!ready || !st_reads) return;
    l_info("[apkcache] %u reads: %u blocks from memory, %u from the card, %u KB read directly; card time %u ms",
           st_reads, st_hits, st_misses, st_direct_kb, st_io_us / 1000);
    st_reads = st_hits = st_misses = st_direct_kb = st_io_us = 0;
}
