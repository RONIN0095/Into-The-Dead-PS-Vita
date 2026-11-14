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
    SceUID uid;      // > 0: address space reserved with kubridge (see below)
    uint8_t *commit; // one bit per page: has memory
    uint32_t *pool;  // per page: its page in the pool (when it has memory)
} mapping;

/*
 * Reservations. Unity 2020.3 reserves its allocator regions with
 * mmap(PROT_NONE) (75 MB at start: three of 16 MB, three of 8 MB, and more
 * later) and gives them memory piece by piece with mprotect or
 * mmap(MAP_FIXED). On Android a reservation costs nothing. Backed by malloc,
 * as everything else here, each one cost its full size, and the game ran out
 * of memory loading its menu. Anonymous PROT_NONE mappings of 1 MB or more
 * are now address space only (kuKernelMemReserve); pages get memory
 * (kuKernelMemCommit) when they are made accessible and lose it when they
 * are unmapped or reserved again. That memory comes from outside the newlib
 * heap, which is smaller for it (main.c).
 */
#include <kubridge.h>
#include <psp2/kernel/sysmem.h>
#define RESERVE_MIN (1024 * 1024)
static size_t committed_bytes = 0, reserved_bytes = 0;

/*
 * The memory comes from a pool: one memory block taken at the first
 * reservation, whose pages are mapped into the reserved ranges
 * (kuKernelMemCommit with a base block). Committing without a base block
 * stopped after 9 MB (0x80024302): the kernel does not give a game that
 * much memory page by page, but it does as one block.
 */
#define POOL_TRIES 5
static const size_t pool_sizes[POOL_TRIES] = { 72u << 20, 64u << 20, 48u << 20, 32u << 20, 24u << 20 };
static SceUID pool_uid = -1;
static size_t pool_pages = 0, pool_free = 0, pool_hint = 0;
static uint8_t *pool_used; // one bit per pool page

static int pool_init(void);

// main.c, before vitaGL starts: vitaGL takes all memory left over for its own
// pool, so the reservations' pool is taken first
void na_mmap_pool_init(void) {
    pool_init();
}

static int pool_init(void) {
    if (pool_uid >= 0) return 1;
    if (pool_uid == -2) return 0;
    for (int i = 0; i < POOL_TRIES; i++) {
        SceUID uid = sceKernelAllocMemBlock("na_mmap_pool", SCE_KERNEL_MEMBLOCK_TYPE_USER_RW, pool_sizes[i], NULL);
        if (uid >= 0) {
            pool_pages = pool_sizes[i] / MAP_PAGE;
            pool_used = calloc(1, (pool_pages + 7) / 8);
            if (!pool_used) {
                sceKernelFreeMemBlock(uid);
                break;
            }
            pool_uid = uid;
            pool_free = pool_pages;
            l_info("mmap: memory pool of %u MB for reservations", (unsigned)(pool_sizes[i] >> 20));
            return 1;
        }
    }
    l_error("mmap: no memory pool for reservations");
    pool_uid = -2;
    return 0;
}

static int pool_bit(size_t i) { return (pool_used[i / 8] >> (i % 8)) & 1; }
static void pool_set(size_t i, int v) {
    if (v) pool_used[i / 8] |= 1 << (i % 8);
    else pool_used[i / 8] &= ~(1 << (i % 8));
}

// A run of up to `want` free pool pages (the first free one found, as long
// as it goes): its first page, and *got pages
static int pool_take(size_t want, size_t *first, size_t *got) {
    if (!pool_free) return 0;
    for (size_t k = 0; k < pool_pages; k++) {
        size_t i = (pool_hint + k) % pool_pages;
        if (pool_bit(i)) continue;
        size_t n = 0;
        while (n < want && i + n < pool_pages && !pool_bit(i + n)) n++;
        for (size_t j = 0; j < n; j++) pool_set(i + j, 1);
        pool_free -= n;
        pool_hint = i + n;
        *first = i;
        *got = n;
        return 1;
    }
    return 0;
}

static int page_bit(const uint8_t *bits, size_t p) { return (bits[p / 8] >> (p % 8)) & 1; }

// Gives the pages of [start, end) memory (zeroed when `zero`, or when new)
static int commit_range(mapping *m, uintptr_t start, uintptr_t end, int zero) {
    if (start < m->base) start = m->base;
    if (end > m->base + m->len) end = m->base + m->len;
    start &= ~(uintptr_t)(MAP_PAGE - 1);
    end = (end + MAP_PAGE - 1) & ~(uintptr_t)(MAP_PAGE - 1);
    int ok = 1;
    for (uintptr_t a = start; a < end;) {
        size_t p = (a - m->base) / MAP_PAGE;
        int have = page_bit(m->commit, p);
        uintptr_t b = a + MAP_PAGE;
        while (b < end && page_bit(m->commit, (b - m->base) / MAP_PAGE) == have) b += MAP_PAGE;
        if (!have) {
            for (uintptr_t c = a; c < b && ok;) {
                size_t first = 0, got = 0;
                int r = -1;
                if (pool_init() && pool_take((b - c) / MAP_PAGE, &first, &got)) {
                    KuKernelMemCommitOpt opt;
                    memset(&opt, 0, sizeof(opt));
                    opt.size = sizeof(opt);
                    opt.attr = KU_KERNEL_MEM_COMMIT_ATTR_HAS_BASE;
                    opt.baseBlock = pool_uid;
                    opt.baseOffset = first * MAP_PAGE;
                    r = kuKernelMemCommit((void *)c, got * MAP_PAGE, KU_KERNEL_PROT_READ | KU_KERNEL_PROT_WRITE, &opt);
                    if (r < 0) {
                        for (size_t j = 0; j < got; j++) pool_set(first + j, 0);
                        pool_free += got;
                    }
                }
                if (r < 0) {
                    static int warned = 0;
                    if (warned++ < 8)
                        l_error("mmap: no memory for %u KB at %p (0x%x; %u KB committed of %u KB reserved, %u KB left in the pool)",
                                (unsigned)((b - c) / 1024), (void *)c, r, (unsigned)(committed_bytes / 1024),
                                (unsigned)(reserved_bytes / 1024), (unsigned)(pool_free * MAP_PAGE / 1024));
                    ok = 0;
                    break;
                }
                for (size_t j = 0; j < got; j++) {
                    size_t i = (c - m->base) / MAP_PAGE + j;
                    m->commit[i / 8] |= 1 << (i % 8);
                    m->pool[i] = (uint32_t)(first + j);
                }
                committed_bytes += got * MAP_PAGE;
                memset((void *)c, 0, got * MAP_PAGE);
                c += got * MAP_PAGE;
            }
        } else if (zero) {
            memset((void *)a, 0, b - a);
        }
        a = b;
    }
    return ok;
}

static void decommit_range(mapping *m, uintptr_t start, uintptr_t end) {
    if (start < m->base) start = m->base;
    if (end > m->base + m->len) end = m->base + m->len;
    start &= ~(uintptr_t)(MAP_PAGE - 1);
    end = (end + MAP_PAGE - 1) & ~(uintptr_t)(MAP_PAGE - 1);
    for (uintptr_t a = start; a < end;) {
        size_t p = (a - m->base) / MAP_PAGE;
        int have = page_bit(m->commit, p);
        uintptr_t b = a + MAP_PAGE;
        while (b < end && page_bit(m->commit, (b - m->base) / MAP_PAGE) == have) b += MAP_PAGE;
        if (have && kuKernelMemDecommit((void *)a, b - a) >= 0) {
            for (uintptr_t q = a; q < b; q += MAP_PAGE) {
                size_t i = (q - m->base) / MAP_PAGE;
                m->commit[i / 8] &= ~(1 << (i % 8));
                pool_set(m->pool[i], 0);
                pool_free++;
            }
            committed_bytes -= b - a;
        }
        a = b;
    }
}

void na_mmap_report(size_t *committed, size_t *reserved) {
    *committed = committed_bytes;
    *reserved = reserved_bytes;
}

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
            if (m->uid > 0) {
                if (prot == 0 && fd < 0) decommit_range(m, (uintptr_t)addr, (uintptr_t)addr + len);
                else if (!commit_range(m, (uintptr_t)addr, (uintptr_t)addr + len, fd < 0)) {
                    unlock();
                    errno = ENOMEM;
                    return MAP_FAILED;
                }
            } else if (fd < 0) memset(ret, 0, len);
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
        if (slot >= 0 && prot == 0 && fd < 0 && len >= RESERVE_MIN) {
            // address space only
            uint8_t *commit = calloc(1, (len / MAP_PAGE + 7) / 8);
            uint32_t *pidx = calloc(len / MAP_PAGE, sizeof(uint32_t));
            gone = calloc(1, (len / MAP_PAGE + 7) / 8);
            void *base = NULL;
            SceUID uid = commit && gone && pidx && pool_init() ? kuKernelMemReserve(&base, len, SCE_KERNEL_MEMBLOCK_TYPE_USER_RW) : -1;
            if (uid > 0 && base) {
                mappings[slot].base = (uintptr_t)base;
                mappings[slot].len = len;
                mappings[slot].gone = gone;
                mappings[slot].n_gone = 0;
                mappings[slot].uid = uid;
                mappings[slot].commit = commit;
                mappings[slot].pool = pidx;
                reserved_bytes += len;
                unlock();
                static int logged_r = 0;
                if (logged_r++ < 48) l_info("mmap: %u KB reserved at %p (no memory yet)", (unsigned)(len / 1024), base);
                return base;
            }
            static int warned = 0;
            if (warned++ < 4) l_warn("mmap: kuKernelMemReserve(%u KB) failed (0x%x): taken from the heap", (unsigned)(len / 1024), uid);
            free(commit);
            free(pidx);
            free(gone);
            gone = NULL;
        }
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
        mappings[slot].uid = 0;
        mappings[slot].commit = NULL;
        mappings[slot].pool = NULL;
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
        if (m->uid > 0) decommit_range(m, (uintptr_t)addr, (uintptr_t)addr + length);
        if (m->n_gone * MAP_PAGE >= m->len) {
            if (m->uid > 0) {
                decommit_range(m, m->base, m->base + m->len);
                sceKernelFreeMemBlock(m->uid);
                reserved_bytes -= m->len;
                free(m->commit);
                free(m->pool);
            } else {
                free((void *)m->base);
            }
            free(m->gone);
            memset(m, 0, sizeof(*m));
        }
    }
    unlock();
    return 0;
}

// mprotect: pages of a reservation get their memory when made accessible
int na_mprotect(void *addr, size_t length, int prot) {
    if (!prot || !length) return 0;
    int ok = 1;
    lock();
    mapping *m = find_mapping((uintptr_t)addr);
    if (m && m->uid > 0) ok = commit_range(m, (uintptr_t)addr, (uintptr_t)addr + length, 0);
    unlock();
    if (!ok) {
        errno = ENOMEM;
        return -1;
    }
    return 0;
}
