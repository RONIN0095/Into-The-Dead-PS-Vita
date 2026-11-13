/*
 * Copyright (C) 2026 Vita port contributors
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

/*
 * Asset files in memory.
 *
 * Loading a level reads 75-85 MB of asset files at the memory card's
 * 11 MB/s: 7 s of a load, and nearly all of it the same shared sprite
 * atlases each time. Asset files of up to 1 MB (the APK's split parts and
 * the small files) are kept whole in memory after their first read, and
 * later reads are copies.
 *
 * - Room: the heap is 248 MB and the game uses 110-130 MB of it. The cache
 *   takes up to BUDGET, grows only while HEADROOM of the heap has never
 *   been touched, and gives half of itself back when less than LOW is left
 *   (the freed blocks go back to malloc, for the game).
 * - What goes: the file not used for the longest time, but none used in
 *   the last PROTECT_US. (A load reads its files in the same order each
 *   time; with too little room, plain least-recently-used throws out
 *   exactly the file needed next and nothing is ever found.) With nothing
 *   to throw out, a new file is not kept.
 * - Ahead of time: the files in the cache are written to preload.txt, and
 *   on the next start a thread reads them in while the menu is up, when
 *   the game itself is not reading. The first level then loads from memory.
 *
 * An open file is looked at once (fstat, no seeking: seeking to the end and
 * back before each read, as the first version did, made the many small
 * reads of the big files four times slower).
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <malloc.h>
#include <pthread.h>
#include <sys/stat.h>

#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>

#include "na_filecache.h"
#include "na_stats.h"
#include "utils/logger.h"

#define MAX_FILE (1024 * 1024)
#define BUDGET (80u * 1024 * 1024)
#define HEADROOM (36u * 1024 * 1024)
#define LOW (16u * 1024 * 1024)
#define PROTECT_US (20 * 1000 * 1000ull)
#define MAX_FILES 320
#define PRELOAD_PATH DATA_PATH "preload.txt"
#define ASSET_DIR "/assets/bin/Data/"

typedef struct {
    char *path;
    uint32_t hash;
    uint8_t *data;
    uint32_t size;
    uint64_t used; // process time of the last read
} entry;

static entry files[MAX_FILES];
static uint32_t total = 0, peak = 0, hits_kb = 0;
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static volatile uint64_t game_read_us = 0; // the game last read the card itself
static volatile int list_dirty = 0, preload_go = 0;

const char *na_io_fd_path(int fd, uint32_t *gen); // reimpl/io.c
extern int _newlib_heap_size_user; // main.c
static char *heap_end = NULL;

// Heap the allocator has never handed out (sbrk has not reached it)
static uint32_t untouched(void) {
    if (!heap_end) {
        struct mallinfo mi = mallinfo(); // once, while the heap is small
        heap_end = (char *)sbrk(0) - mi.arena + _newlib_heap_size_user;
    }
    char *brk = (char *)sbrk(0);
    return brk < heap_end ? (uint32_t)(heap_end - brk) : 0;
}

static uint32_t hash_of(const char *s) {
    uint32_t h = 2166136261u;
    while (*s) h = (h ^ (uint8_t)*s++) * 16777619u;
    return h;
}

static entry *find(const char *path, uint32_t h) {
    for (int i = 0; i < MAX_FILES; i++)
        if (files[i].data && files[i].hash == h && strcmp(files[i].path, path) == 0) return &files[i];
    return NULL;
}

static void drop(entry *e) {
    total -= e->size;
    free(e->data);
    free(e->path);
    memset(e, 0, sizeof(*e));
}

// The file not used for the longest time, if that is longer ago than `age`
static int evict_one(uint64_t age) {
    uint64_t now = sceKernelGetProcessTimeWide();
    entry *old = NULL;
    for (int i = 0; i < MAX_FILES; i++)
        if (files[i].data && (!old || files[i].used < old->used)) old = &files[i];
    if (!old || now - old->used < age) return 0;
    drop(old);
    return 1;
}

static void trim_locked(void) {
    if (!total || untouched() >= LOW) return;
    uint32_t goal = total / 2, before = total;
    while (total > goal && evict_one(0)) {}
    peak = total;
    list_dirty = 1;
    l_info("[cache] heap tight: asset cache %u -> %u KB", (unsigned)(before / 1024), (unsigned)(total / 1024));
}

// Makes room for `size` more bytes; 0 when the file should not be kept
static int make_room(uint32_t size) {
    trim_locked();
    while (total + size > BUDGET)
        if (!evict_one(PROTECT_US)) return 0;
    uint32_t free_now = untouched();
    if (total + size > peak ? free_now < HEADROOM + size : free_now < LOW + size) return 0;
    for (int i = 0; i < MAX_FILES; i++)
        if (!files[i].data) return 1;
    return evict_one(PROTECT_US);
}

static void insert(const char *path, uint32_t h, uint8_t *data, uint32_t size) {
    for (int i = 0; i < MAX_FILES; i++) {
        if (files[i].data) continue;
        char *p = strdup(path);
        if (!p) break;
        files[i].path = p;
        files[i].hash = h;
        files[i].data = data;
        files[i].size = size;
        files[i].used = sceKernelGetProcessTimeWide();
        total += size;
        if (total > peak) peak = total;
        list_dirty = 1;
        return;
    }
    free(data);
}

void na_filecache_drop(void) {
    pthread_mutex_lock(&lock);
    for (int i = 0; i < MAX_FILES; i++)
        if (files[i].data) drop(&files[i]);
    pthread_mutex_unlock(&lock);
}

uint32_t na_filecache_kb(uint32_t *hits) {
    if (hits) *hits = hits_kb;
    return total / 1024;
}

void na_filecache_game_read(void) {
    game_read_us = sceKernelGetProcessTimeWide();
}

// What is known about an open file
#define MAX_FD 256
static struct {
    uint32_t gen;
    uint8_t state; // 0 not looked at, 1 kept in memory when there is room, 2 read the usual way
    uint32_t size, hash;
} fds[MAX_FD];

int na_filecache_read(int fd, void *buf, size_t n, long *out) {
    if (fd < 0 || fd >= MAX_FD) return 0;
    uint32_t gen = 0;
    const char *path = na_io_fd_path(fd, &gen);
    if (!path) return 0;
    if (fds[fd].gen != gen || !fds[fd].state) {
        fds[fd].gen = gen;
        fds[fd].state = 2;
        struct stat st;
        if (strstr(path, ASSET_DIR) && fstat(fd, &st) == 0 && st.st_size > 0 && st.st_size <= MAX_FILE) {
            fds[fd].state = 1;
            fds[fd].size = (uint32_t)st.st_size;
            fds[fd].hash = hash_of(path);
        }
    }
    if (fds[fd].state != 1) return 0;

    int served = 0;
    pthread_mutex_lock(&lock);
    trim_locked();
    entry *e = find(path, fds[fd].hash);
    long pos = (long)lseek(fd, 0, SEEK_CUR);
    if (!e && pos >= 0) {
        uint32_t size = fds[fd].size;
        uint8_t *d = make_room(size) ? malloc(size) : NULL;
        if (d) {
            uint64_t t0 = sceKernelGetProcessTimeWide();
            uint32_t got = 0;
            lseek(fd, 0, SEEK_SET);
            while (got < size) {
                long r = read(fd, d + got, size - got);
                if (r <= 0) break;
                got += (uint32_t)r;
            }
            lseek(fd, pos, SEEK_SET);
            game_read_us = sceKernelGetProcessTimeWide();
            na_stats.read_us += (uint32_t)(game_read_us - t0);
            na_stats.read_kb += got / 1024;
            if (got == size) {
                insert(path, fds[fd].hash, d, size);
                e = find(path, fds[fd].hash);
            } else {
                free(d);
            }
        }
        if (!e) fds[fd].state = 2; // no room (or no luck): the usual way for this file
    }
    if (e && pos >= 0) {
        e->used = sceKernelGetProcessTimeWide();
        size_t left = (uint32_t)pos < e->size ? e->size - (uint32_t)pos : 0;
        size_t k = n < left ? n : left;
        memcpy(buf, e->data + pos, k);
        // like a read: the position moves on (to the end after a short read)
        lseek(fd, k < n ? (long)e->size : pos + (long)k, SEEK_SET);
        hits_kb += (uint32_t)(k / 1024);
        *out = (long)k;
        served = 1;
    }
    pthread_mutex_unlock(&lock);
    return served;
}

/*
 * The thread: reads last time's files in while the game is not reading,
 * and writes the list of what is in the cache when it has changed.
 */
static void wait_idle(uint64_t quiet_us) {
    while (sceKernelGetProcessTimeWide() - game_read_us < quiet_us) sceKernelDelayThread(100 * 1000);
}

static void preload(void) {
    FILE *f = fopen(PRELOAD_PATH, "r");
    if (!f) return;
    char (*list)[160] = malloc(MAX_FILES * 160);
    int n = 0;
    if (list) {
        while (n < MAX_FILES && fgets(list[n], 160, f)) {
            list[n][strcspn(list[n], "\r\n")] = 0;
            if (strstr(list[n], ASSET_DIR)) n++;
        }
    }
    fclose(f);
    if (!list) return;
    uint64_t t0 = sceKernelGetProcessTimeWide();
    uint32_t done_kb = 0;
    int done = 0;
    for (int i = 0; i < n; i++) {
        wait_idle(700 * 1000);
        uint32_t h = hash_of(list[i]);
        pthread_mutex_lock(&lock);
        int have = find(list[i], h) != NULL;
        pthread_mutex_unlock(&lock);
        if (have) continue;
        int fd = open(list[i], O_RDONLY);
        if (fd < 0) continue;
        struct stat st;
        uint8_t *d = NULL;
        uint32_t size = 0;
        if (fstat(fd, &st) == 0 && st.st_size > 0 && st.st_size <= MAX_FILE) {
            size = (uint32_t)st.st_size;
            pthread_mutex_lock(&lock);
            int room = make_room(size);
            pthread_mutex_unlock(&lock);
            if (!room) {
                close(fd);
                break; // full: the rest would not fit either
            }
            d = malloc(size);
        }
        uint32_t got = 0;
        while (d && got < size) {
            long r = read(fd, d + got, size - got);
            if (r <= 0) break;
            got += (uint32_t)r;
        }
        close(fd);
        if (!d) continue;
        if (got != size) {
            free(d);
            continue;
        }
        pthread_mutex_lock(&lock);
        if (find(list[i], h) || !make_room(size)) free(d); // the game got there first, or no room now
        else {
            insert(list[i], h, d, size);
            done++;
            done_kb += size / 1024;
        }
        pthread_mutex_unlock(&lock);
    }
    free(list);
    l_info("[cache] %d of %d asset files read ahead (%u KB) in %u ms", done, n, (unsigned)done_kb,
           (unsigned)((sceKernelGetProcessTimeWide() - t0) / 1000));
}

static int by_used(const void *a, const void *b) {
    const entry *x = *(entry *const *)a, *y = *(entry *const *)b;
    return x->used < y->used ? 1 : x->used > y->used ? -1 : 0; // most recent first
}

static void save_list(void) {
    pthread_mutex_lock(&lock);
    list_dirty = 0;
    entry *order[MAX_FILES];
    int n = 0;
    size_t cap = 1;
    for (int i = 0; i < MAX_FILES; i++)
        if (files[i].data) {
            order[n++] = &files[i];
            cap += strlen(files[i].path) + 1;
        }
    qsort(order, n, sizeof(order[0]), by_used);
    char *text = malloc(cap);
    size_t len = 0;
    if (text)
        for (int i = 0; i < n; i++) len += snprintf(text + len, cap - len, "%s\n", order[i]->path);
    pthread_mutex_unlock(&lock);
    if (!text) return;
    FILE *f = fopen(PRELOAD_PATH ".tmp", "w");
    if (f) {
        int ok = fwrite(text, 1, len, f) == len;
        ok = fclose(f) == 0 && ok;
        if (ok) {
            remove(PRELOAD_PATH);
            rename(PRELOAD_PATH ".tmp", PRELOAD_PATH);
        }
    }
    free(text);
}

static void *cache_thread(void *arg) {
    (void)arg;
    while (!preload_go) sceKernelDelayThread(200 * 1000);
    preload();
    list_dirty = 0; // what was read ahead is the list as it is
    for (;;) {
        sceKernelDelayThread(2000 * 1000);
        if (!list_dirty) continue;
        wait_idle(5000 * 1000); // not in the middle of a load
        save_list();
    }
    return NULL;
}

void na_filecache_tick(int frame) {
    static int started = 0;
    if (!started) {
        started = 1;
        pthread_t th;
        pthread_attr_t a;
        pthread_attr_init(&a);
        pthread_attr_setstacksize(&a, 128 * 1024);
        pthread_create(&th, &a, cache_thread, NULL);
        pthread_attr_destroy(&a);
    }
    if (frame == 200) preload_go = 1; // the menu is up
    if (frame % 30 == 15) {
        pthread_mutex_lock(&lock);
        trim_locked();
        pthread_mutex_unlock(&lock);
    }
}
