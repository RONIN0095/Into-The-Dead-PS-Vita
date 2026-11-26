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
 *
 * Reading ahead: the card took ~8.5 ms for each 64 KB block (7.5 MB/s;
 * starting the game read 55 MB that way). Unity reads its bundles mostly
 * front to back, so a miss on the block after the last one read loads
 * RUN blocks in one read, and a thread reads the AHEAD blocks after those
 * while Unity unpacks what it has. A miss on a block that thread is still
 * reading waits for it instead of reading it twice.
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

#include "reimpl/mem.h"
#include "utils/logger.h"

#define BLOCK (64 * 1024)
#define DIRECT_MIN (256 * 1024) // reads this big skip the cache (their middle part)
#define RUN 4                   // blocks read at once on a sequential miss
#define AHEAD 8                 // blocks the read-ahead thread loads after those

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
static uint32_t st_reads, st_hits, st_misses, st_direct_kb, st_io_us, st_ahead, st_waits;

static uint8_t *run_buf, *ahead_buf;   // RUN and AHEAD blocks
static int64_t last_blk = -2;           // last block a miss or hit was on
static volatile int64_t ahead_from = -1; // blocks the thread is reading: [ahead_from, ahead_from + AHEAD)
static SceUID ahead_sema = -1;

static void cache_init(void) {
    if (ready) return;
    sceKernelCreateLwMutex(&lock_work, "apkcache", 0, 0, NULL);
    /*
     * 6 MB from the reservations' pool (reimpl/mem.c). A
     * memory block of its own (8 MB) took the memory the kernel needed for
     * Unity's next thread: pthread_create failed and the game hung black.
     */
    size_t sz = 5u << 20; // (3 MB: of each load's ~1000 blocks about 950 still came from the card)
    void *p = mmap(NULL, sz, 3 /* PROT_READ | PROT_WRITE */, 0x22 /* bionic MAP_PRIVATE | MAP_ANONYMOUS */, -1, 0);
    if (p && p != (void *)-1) {
        memset(p, 0, sz); // its pages get memory now (reads from the card can't fault them in)
        cache_mem = p;
        n_slots = (int)(sz / BLOCK);
    }
    void *q = mmap(NULL, (RUN + AHEAD) * BLOCK, 3, 0x22, -1, 0);
    if (q && q != (void *)-1) {
        memset(q, 0, (RUN + AHEAD) * BLOCK);
        run_buf = q;
        ahead_buf = run_buf + RUN * BLOCK;
    }
    slots = calloc(n_slots ? n_slots : 1, sizeof(slot));
    for (int i = 0; i < n_slots; i++) slots[i].block = -1;
    ready = 1;
    l_info("[apkcache] %d KB of 64 KB blocks for base.apk, reading ahead %s", n_slots * BLOCK / 1024,
           run_buf ? "on" : "off");
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

static int find_slot(int64_t blk) {
    for (int i = 0; i < n_slots; i++)
        if (slots[i].block == blk) return i;
    return -1;
}

static int victim_slot(void) {
    int victim = 0;
    uint32_t oldest = 0xffffffff;
    for (int i = 0; i < n_slots; i++)
        if (slots[i].used < oldest) oldest = slots[i].used, victim = i;
    return victim;
}

// Puts len bytes of block blk (from a run read) into a slot, if it isn't there
static void put_block(int64_t blk, const uint8_t *data, int len) {
    if (len <= 0 || find_slot(blk) >= 0) return;
    int v = victim_slot();
    memcpy(cache_mem + (size_t)v * BLOCK, data, len);
    slots[v].block = blk;
    slots[v].used = ++clock_tick;
}

static int blk_len(int64_t blk) {
    int64_t left = apk_size - blk * BLOCK;
    return left < BLOCK ? (left > 0 ? (int)left : 0) : BLOCK;
}

// The read-ahead thread: reads AHEAD blocks from ahead_from, then puts them in
static int ahead_thread(SceSize args, void *argp) {
    for (;;) {
        sceKernelWaitSema(ahead_sema, 1, NULL);
        int64_t from = ahead_from;
        if (from < 0) continue;
        int64_t bytes = apk_size - from * BLOCK;
        if (bytes > (int64_t)AHEAD * BLOCK) bytes = (int64_t)AHEAD * BLOCK;
        int r = bytes > 0 ? read_at(ahead_buf, (size_t)bytes, from * BLOCK) : 0;
        sceKernelLockLwMutex(&lock_work, 1, NULL);
        for (int i = 0; i * BLOCK < r; i++) {
            int len = r - i * BLOCK < BLOCK ? r - i * BLOCK : BLOCK;
            put_block(from + i, ahead_buf + (size_t)i * BLOCK, len);
        }
        st_ahead += r > 0 ? (r + BLOCK - 1) / BLOCK : 0;
        ahead_from = -1;
        sceKernelUnlockLwMutex(&lock_work, 1);
    }
    return 0;
}

static void start_ahead(int64_t from) {
    if (ahead_sema < 0 || ahead_from >= 0 || from * BLOCK >= apk_size) return;
    // nothing to do if those blocks are all here already
    int missing = 0;
    for (int i = 0; i < AHEAD && !missing; i++) missing = find_slot(from + i) < 0;
    if (!missing) return;
    ahead_from = from;
    sceKernelSignalSema(ahead_sema, 1);
}

// The block's data (at most BLOCK bytes, fewer at the end); *len its length.
// Called with the lock held; may drop it while the read-ahead thread works.
static uint8_t *get_block(int64_t blk, int *len) {
    for (;;) {
        int i = find_slot(blk);
        if (i >= 0) {
            slots[i].used = ++clock_tick;
            st_hits++;
            *len = blk_len(blk);
            // reading on in order: keep the thread ahead of the reader
            if (blk == last_blk + 1 && run_buf) start_ahead(blk + 1 + RUN);
            last_blk = blk;
            return cache_mem + (size_t)i * BLOCK;
        }
        int64_t af = ahead_from;
        if (af >= 0 && blk >= af && blk < af + AHEAD) {
            // the thread is reading it: wait for it
            st_waits++;
            sceKernelUnlockLwMutex(&lock_work, 1);
            while (ahead_from == af) sceKernelDelayThread(200);
            sceKernelLockLwMutex(&lock_work, 1, NULL);
            continue;
        }
        break;
    }
    st_misses++;
    int sequential = run_buf && blk == last_blk + 1;
    last_blk = blk;
    if (sequential) {
        // this and the next RUN-1 blocks in one read, then the thread goes on after them
        int64_t bytes = apk_size - blk * BLOCK;
        if (bytes > (int64_t)RUN * BLOCK) bytes = (int64_t)RUN * BLOCK;
        int r = read_at(run_buf, (size_t)bytes, blk * BLOCK);
        if (r <= 0) {
            *len = 0;
            return NULL;
        }
        for (int i = 0; i * BLOCK < r; i++) {
            int l = r - i * BLOCK < BLOCK ? r - i * BLOCK : BLOCK;
            put_block(blk + i, run_buf + (size_t)i * BLOCK, l);
        }
        start_ahead(blk + RUN);
        int k = find_slot(blk);
        if (k < 0) {
            *len = 0;
            return NULL;
        }
        *len = blk_len(blk);
        return cache_mem + (size_t)k * BLOCK;
    }
    int victim = victim_slot();
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
        if (run_buf) {
            ahead_sema = sceKernelCreateSema("apkahead", 0, 0, 1, NULL);
            SceUID t = sceKernelCreateThread("apkahead", ahead_thread, 0x10000100 - 8, 0x4000, 0, 0, NULL);
            if (ahead_sema < 0 || t < 0 || sceKernelStartThread(t, 0, NULL) < 0) {
                l_info("[apkcache] no read-ahead thread (0x%x 0x%x)", ahead_sema, t);
                ahead_sema = -1;
            }
        }
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
    l_info("[apkcache] %u reads: %u blocks from memory, %u from the card, %u read ahead (%u waits), %u KB read directly; card time %u ms",
           st_reads, st_hits, st_misses, st_ahead, st_waits, st_direct_kb, st_io_us / 1000);
    st_reads = st_hits = st_misses = st_direct_kb = st_io_us = st_ahead = st_waits = 0;
}
