/*
 * Copyright (C) 2021      Andy Nguyen
 * Copyright (C) 2022      Rinnegatamante
 * Copyright (C) 2022-2023 Volodymyr Atamanenko
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

#include "reimpl/mem.h"
#include "utils/logger.h"

#include <string.h>
#include <malloc.h>
#include <psp2/kernel/clib.h>

void *sceClibMemclr(void *dst, size_t len) {
    return sceClibMemset(dst, 0, len);
}

/*
 * mmap/munmap on top of malloc.
 *
 * Unity reserves memory with mmap and then munmaps the unaligned head and
 * tail of the block to get an aligned region, so munmap gets addresses inside
 * a mapping. Mappings are tracked page by page (a page unmapped twice counts
 * once, MAP_FIXED maps it back) and freed once every page is unmapped. File
 * mappings are read into memory.
 */

#include <errno.h>
#include <unistd.h>
#include <psp2/kernel/threadmgr.h>

#define MAP_FIXED_BIONIC 0x10
#define MAP_ANON_BIONIC 0x20
#define MAX_MAPPINGS 1024
#define MAP_PAGE 4096

typedef struct {
    uintptr_t base;
    size_t len;
    uint8_t *gone;   // one bit per page: unmapped
    size_t n_gone;   // pages unmapped
} mapping;

static void mark_pages(mapping *m, uintptr_t start, uintptr_t end, int unmapped) {
    if (start < m->base) start = m->base;
    if (end > m->base + m->len) end = m->base + m->len;
    for (uintptr_t a = start & ~(uintptr_t)(MAP_PAGE - 1); a < end; a += MAP_PAGE) {
        size_t p = (a - m->base) / MAP_PAGE;
        int was = (m->gone[p / 8] >> (p % 8)) & 1;
        if (unmapped && !was) {
            m->gone[p / 8] |= 1 << (p % 8);
            m->n_gone++;
        } else if (!unmapped && was) {
            m->gone[p / 8] &= ~(1 << (p % 8));
            m->n_gone--;
        }
    }
}

static mapping mappings[MAX_MAPPINGS];
static SceKernelLwMutexWork map_lock;
static volatile int map_lock_ready = 0;

static void lock(void) {
    if (!map_lock_ready) {
        // first call is on the main thread (library constructors)
        sceKernelCreateLwMutex(&map_lock, "mmap", 0, 0, NULL);
        map_lock_ready = 1;
    }
    sceKernelLockLwMutex(&map_lock, 1, NULL);
}

static void unlock(void) {
    sceKernelUnlockLwMutex(&map_lock, 1);
}

static mapping *find_mapping(uintptr_t a) {
    for (int i = 0; i < MAX_MAPPINGS; i++)
        if (mappings[i].base && a >= mappings[i].base && a < mappings[i].base + mappings[i].len)
            return &mappings[i];
    return NULL;
}

void *mmap(void *addr, size_t length, int prot, int flags, int fd, off_t offs) {
    if (length == 0) {
        errno = EINVAL;
        return MAP_FAILED;
    }
    size_t len = (length + MAP_PAGE - 1) & ~(size_t)(MAP_PAGE - 1);
    void *ret = NULL;

    lock();
    if (flags & MAP_FIXED_BIONIC) {
        mapping *m = find_mapping((uintptr_t)addr);
        if (m && (uintptr_t)addr + len <= m->base + m->len) {
            ret = addr;
            mark_pages(m, (uintptr_t)addr, (uintptr_t)addr + len, 0);
            if (fd < 0) memset(ret, 0, len);
        } else {
            unlock();
            l_error("mmap(%p, %u, MAP_FIXED): not inside a mapping", addr, (unsigned)length);
            errno = ENOMEM;
            return MAP_FAILED;
        }
    } else {
        int slot = -1;
        for (int i = 0; i < MAX_MAPPINGS; i++)
            if (!mappings[i].base) { slot = i; break; }
        uint8_t *gone = NULL;
        if (slot >= 0) {
            gone = calloc(1, (len / MAP_PAGE + 7) / 8);
            ret = gone ? memalign(MAP_PAGE, len) : NULL;
        }
        if (!ret) {
            free(gone);
            unlock();
            l_error("mmap(%u bytes): out of memory%s", (unsigned)length, slot < 0 ? " (mapping table full)" : "");
            errno = ENOMEM;
            return MAP_FAILED;
        }
        memset(ret, 0, len);
        mappings[slot].base = (uintptr_t)ret;
        mappings[slot].len = len;
        mappings[slot].gone = gone;
        mappings[slot].n_gone = 0;
    }
    unlock();

    if (fd >= 0) {
        off_t old = lseek(fd, 0, SEEK_CUR);
        lseek(fd, offs, SEEK_SET);
        size_t got = 0;
        while (got < length) { // read() may return less than asked
            int r = read(fd, (uint8_t *)ret + got, length - got);
            if (r <= 0) break;
            got += r;
        }
        lseek(fd, old, SEEK_SET);
        static int file_logged = 0;
        if (file_logged++ < 100)
            l_info("mmap(fd %d, off %ld, %u bytes): read %u", fd, (long)offs, (unsigned)length, (unsigned)got);
    }

    static int logged = 0;
    if (logged++ < 32)
        l_info("mmap(%p, %u, prot %d, flags 0x%x, fd %d, off %ld) = %p", addr, (unsigned)length, prot, flags, fd, (long)offs, ret);
    return ret;
}

int munmap(void *addr, size_t length) {
    lock();
    mapping *m = find_mapping((uintptr_t)addr);
    if (m) {
        // pages are counted once even if unmapped repeatedly; the memory is
        // freed when every page of the mapping is unmapped
        mark_pages(m, (uintptr_t)addr, (uintptr_t)addr + length, 1);
        if (m->n_gone * MAP_PAGE >= m->len) {
            free((void *)m->base);
            free(m->gone);
            memset(m, 0, sizeof(*m));
        }
    }
    unlock();
    return 0;
}
