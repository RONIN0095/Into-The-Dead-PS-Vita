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
#define MAX_MAPPINGS 4096
#define MAP_PAGE 4096

typedef struct mapping_s {
    uintptr_t base;
    size_t len;
    uint8_t *gone;   // one bit per page: unmapped
    size_t n_gone;   // pages unmapped
    SceUID uid;      // > 0: address space reserved with kubridge (see below)
    uint8_t *commit; // one bit per page: has memory
    uint32_t *pool;  // per page: its page in the pool (when it has memory)
    uint8_t *access; // one bit per page: made accessible (gets memory on first touch)
    int dead;        // reservation unmapped: kept for reuse (see munmap)
    // Lending (see lend_from_dead): a dead reservation lends ranges of its
    // pages, memory included, to new reservations
    struct mapping_s *parent; // the reservation this one borrows from
    size_t parent_page;       // its first page in the parent
    uint8_t *lent;            // parent: one bit per page lent out
    size_t n_lent;            // parent: pages lent out
    int arena;                // the arena for small mappings (see arena_map)
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
static size_t madvise_calls = 0, madvise_bytes = 0, munmap_calls = 0, munmap_bytes = 0, plain_mmap_bytes = 0;

/*
 * The memory comes from a pool: one memory block taken at the first
 * reservation, whose pages are mapped into the reserved ranges
 * (kuKernelMemCommit with a base block). Committing without a base block
 * stopped after 9 MB (0x80024302): the kernel does not give a game that
 * much memory page by page, but it does as one block.
 */
/*
 * The pool is made of blocks of 16 MB, as many as there are (up to
 * POOL_TARGET), taken before vitaGL starts (vitaGL keeps what is left over
 * for itself). Unity 2020.3 takes nearly all its memory through these
 * reservations: with 72 MB of pool and a 176 MB heap, the heap had 36 MB
 * in use when the pool ran out loading the menu. The heap is 80 MB now
 * (main.c) and the pool gets the rest.
 */
#define POOL_BLOCK (16u << 20)
#define BLOCK_PAGES (POOL_BLOCK / MAP_PAGE)
#define MAX_BLOCKS 20
#define POOL_TARGET (184u << 20) // 160 MB ran out in the intro scene; the heap gave 24 MB
static SceUID pool_uids[MAX_BLOCKS];
static int pool_blocks = 0, pool_state = 0; // 0 not tried, 1 ready, -1 none
static size_t pool_pages = 0, pool_free = 0, pool_hint = 0;
static uint8_t pool_used[MAX_BLOCKS * BLOCK_PAGES / 8]; // one bit per pool page

static int pool_init(void);
static int pool_grow(void);
static void pool_set(size_t i, int v);

// main.c, before vitaGL starts: vitaGL takes all memory left over for its own
// pool, so the reservations' pool is taken first
void na_mmap_pool_init(void) {
    pool_init();
}

/*
 * More pool from the GPU's memory. Main memory is what runs out (libraries,
 * heap and 160 MB of pool fill it), while of the 128 MB of CDRAM the game's
 * textures need well under half. CDRAM blocks can't be used as they are
 * (they are mapped uncached, far too slow for a heap), but their physical
 * pages can back a reservation like any pool block's, and there they are
 * mapped like normal memory. Each block is tried first: one of its pages is
 * committed into a small test reservation and must read back what was
 * written through the block's own mapping. If kubridge refuses (or the
 * page doesn't read back), the block goes back to the GPU.
 */
// One block only: with three (48 MB) the pool had 67 MB to spare while the
// GPU's memory ran out in the intro scene
// Two again, and a phycont block as well (below): with Unity's textures at
// half size (na_hud.c) the GPU's memory had 21 MB free, vitaGL's own 17 MB
// and all 26 MB of phycont were unused when the pool ran out after a run.
#define POOL_CDRAM_BLOCKS 2
static int pool_cdram_blocks = 0;
static size_t pool_reserved_pages = 0; // pool pages used for the tests

static SceUID trial_uid = -1;
static int cdram_block_works(SceUID uid, int index) {
    static void *trial = NULL;
    static int trial_n = 0;
    if (!trial) {
        SceUID r = kuKernelMemReserve(&trial, 1024 * 1024, SCE_KERNEL_MEMBLOCK_TYPE_USER_RW);
        trial_uid = r;
        if (r < 0 || !trial) {
            l_warn("mmap: no test reservation for CDRAM pool blocks (0x%08x)", (unsigned)r);
            trial = NULL;
            return 0;
        }
    }
    void *blk = NULL;
    if (sceKernelGetMemBlockBase(uid, &blk) < 0 || !blk) return 0;
    size_t off = POOL_BLOCK - MAP_PAGE; // the block's last page
    volatile uint32_t *direct = (volatile uint32_t *)((uintptr_t)blk + off);
    uint32_t pattern = 0xC0DE0000u + (uint32_t)index;
    direct[0] = pattern;
    direct[1023] = ~pattern;
    void *at = (void *)((uintptr_t)trial + (size_t)trial_n++ * MAP_PAGE);
    KuKernelMemCommitOpt opt;
    memset(&opt, 0, sizeof(opt));
    opt.size = sizeof(opt);
    opt.attr = KU_KERNEL_MEM_COMMIT_ATTR_HAS_BASE;
    opt.baseBlock = uid;
    opt.baseOffset = off;
    int r = kuKernelMemCommit(at, MAP_PAGE, KU_KERNEL_PROT_READ | KU_KERNEL_PROT_WRITE, &opt);
    if (r < 0) {
        l_warn("mmap: kubridge can't map this block's pages into a reservation (0x%08x)", (unsigned)r);
        return 0;
    }
    volatile uint32_t *mapped = at;
    if (mapped[0] != pattern || mapped[1023] != ~pattern) {
        l_warn("mmap: the block's page mapped into a reservation reads %08x, not %08x", (unsigned)mapped[0],
               (unsigned)pattern);
        return 0;
    }
    mapped[0] = 0;
    mapped[1023] = 0;
    return 1;
}

static int pool_init(void) {
    if (pool_state) return pool_state > 0;
    while (pool_blocks < MAX_BLOCKS && (size_t)pool_blocks * POOL_BLOCK < POOL_TARGET) {
        SceUID uid = sceKernelAllocMemBlock("na_mmap_pool", SCE_KERNEL_MEMBLOCK_TYPE_USER_RW, POOL_BLOCK, NULL);
        if (uid < 0) break;
        pool_uids[pool_blocks++] = uid;
    }
    int main_blocks = pool_blocks, phycont_blocks = 0;
    // CDRAM first; if that can't be used, one block of the 26 MB of
    // physically contiguous memory (vitaGL's last resort for textures)
    for (int kind = 0; kind < 2; kind++) {
        int want = kind == 0 ? POOL_CDRAM_BLOCKS : 1;
        for (int n = 0; n < want && pool_blocks < MAX_BLOCKS; n++) {
            SceUID uid = sceKernelAllocMemBlock(kind == 0 ? "na_mmap_pool_cd" : "na_mmap_pool_pc",
                                                kind == 0 ? SCE_KERNEL_MEMBLOCK_TYPE_USER_CDRAM_RW
                                                          : SCE_KERNEL_MEMBLOCK_TYPE_USER_MAIN_PHYCONT_RW,
                                                POOL_BLOCK, NULL);
            if (uid < 0) {
                l_warn("mmap: no %s block for the pool (0x%08x)", kind == 0 ? "CDRAM" : "phycont", (unsigned)uid);
                break;
            }
            if (!cdram_block_works(uid, pool_blocks)) {
                sceKernelFreeMemBlock(uid);
                break;
            }
            // its last page stays in the test reservation
            pool_set((size_t)pool_blocks * BLOCK_PAGES + BLOCK_PAGES - 1, 1);
            pool_reserved_pages++;
            pool_uids[pool_blocks++] = uid;
            if (kind == 0) pool_cdram_blocks++;
            else phycont_blocks++;
        }
    }
    pool_pages = (size_t)pool_blocks * BLOCK_PAGES;
    pool_free = pool_pages - pool_reserved_pages;
    pool_state = pool_blocks ? 1 : -1;
    if (pool_blocks)
        l_info("mmap: memory pool of %u MB for reservations (%u MB main memory, %u MB CDRAM, %u MB phycont)",
               (unsigned)(pool_blocks * (POOL_BLOCK >> 20)), (unsigned)(main_blocks * (POOL_BLOCK >> 20)),
               (unsigned)(pool_cdram_blocks * (POOL_BLOCK >> 20)), (unsigned)(phycont_blocks * (POOL_BLOCK >> 20)));
    else
        l_error("mmap: no memory pool for reservations");
    return pool_state > 0;
}

/*
 * The pool used up: one more block, of the physically contiguous memory
 * (26 MB that only vitaGL's last resort for textures uses; 17 MB of it were
 * free when a run started from the menu found the pool empty).
 */
static int pool_grow(void) {
    static int tried = 0;
    if (tried || pool_blocks >= MAX_BLOCKS) return 0;
    tried = 1;
    SceUID uid = sceKernelAllocMemBlock("na_mmap_pool_pc", SCE_KERNEL_MEMBLOCK_TYPE_USER_MAIN_PHYCONT_RW, POOL_BLOCK, NULL);
    if (uid < 0) {
        l_warn("mmap: the pool is used up and there is no phycont block for it (0x%08x)", (unsigned)uid);
        return 0;
    }
    if (!cdram_block_works(uid, pool_blocks)) {
        sceKernelFreeMemBlock(uid);
        return 0;
    }
    pool_set((size_t)pool_blocks * BLOCK_PAGES + BLOCK_PAGES - 1, 1); // stays in the test reservation
    pool_reserved_pages++;
    pool_uids[pool_blocks++] = uid;
    pool_pages = (size_t)pool_blocks * BLOCK_PAGES;
    pool_free += BLOCK_PAGES - 1;
    l_warn("mmap: the pool was used up: 16 MB of phycont memory added (%d blocks now)", pool_blocks);
    return 1;
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
        size_t n = 0, block_end = (i / BLOCK_PAGES + 1) * BLOCK_PAGES; // a run stays in one block
        while (n < want && i + n < block_end && !pool_bit(i + n)) n++;
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

#define NO_POOL_PAGE 0xFFFFFFFFu
#define PLAIN_MAX_PAGES (6u * 1024 * 1024 / MAP_PAGE)
static size_t plain_pages = 0;

// Gives the pages of [start, end) memory (zeroed when `zero`, or when new)
static int commit_range(mapping *m, uintptr_t start, uintptr_t end, int zero, int only_access) {
    if (start < m->base) start = m->base;
    if (end > m->base + m->len) end = m->base + m->len;
    start &= ~(uintptr_t)(MAP_PAGE - 1);
    end = (end + MAP_PAGE - 1) & ~(uintptr_t)(MAP_PAGE - 1);
    int ok = 1;
    for (uintptr_t a = start; a < end;) {
        size_t p = (a - m->base) / MAP_PAGE;
        int have = page_bit(m->commit, p) || (only_access && !page_bit(m->access, p));
#define SKIP_PAGE(q) (page_bit(m->commit, ((q) - m->base) / MAP_PAGE) || (only_access && !page_bit(m->access, ((q) - m->base) / MAP_PAGE)))
        uintptr_t b = a + MAP_PAGE;
        while (b < end && SKIP_PAGE(b) == have) b += MAP_PAGE;
#undef SKIP_PAGE
        if (!have) {
            for (uintptr_t c = a; c < b && ok;) {
                size_t first = 0, got = 0;
                int r = -1, plain = 0;
                if (pool_init() && !pool_free) pool_grow();
                if (!(pool_init() && pool_free) && plain_pages < PLAIN_MAX_PAGES) {
                    // The pool is used up: the last few MB come from the
                    // kernel page by page (what vitaGL left free)
                    got = (b - c) / MAP_PAGE;
                    if (got > 4) got = 4;
                    r = kuKernelMemCommit((void *)c, got * MAP_PAGE, KU_KERNEL_PROT_READ | KU_KERNEL_PROT_WRITE, NULL);
                    if (r >= 0) {
                        if (!plain_pages) l_warn("mmap: the pool is used up, taking pages from the kernel");
                        plain_pages += got;
                        first = 0;
                        plain = 1;
                    } else {
                        plain_pages = PLAIN_MAX_PAGES; // the kernel has none either
                    }
                } else if (pool_init() && pool_take((b - c) / MAP_PAGE, &first, &got)) {
                    KuKernelMemCommitOpt opt;
                    memset(&opt, 0, sizeof(opt));
                    opt.size = sizeof(opt);
                    opt.attr = KU_KERNEL_MEM_COMMIT_ATTR_HAS_BASE;
                    opt.baseBlock = pool_uids[first / BLOCK_PAGES];
                    opt.baseOffset = (first % BLOCK_PAGES) * MAP_PAGE;
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
                    m->pool[i] = plain ? NO_POOL_PAGE : (uint32_t)(first + j);
                }
                committed_bytes += got * MAP_PAGE;
                memset((void *)c, 0, got * MAP_PAGE);
                c += got * MAP_PAGE;
            }
        } else if (zero && !only_access) {
            memset((void *)a, 0, b - a);
        }
        a = b;
    }
    return ok;
}

/*
 * Pages a reservation no longer needs keep their memory, zeroed (they read
 * as zero when used again, as fresh anonymous pages do). Giving it back with
 * kuKernelMemDecommit hung the calling thread in the kernel (Unity's
 * madvise, then its munmap, while the menu loaded) and left the console
 * unstable, so the port never decommits.
 */
static void decommit_range(mapping *m, uintptr_t start, uintptr_t end) {
    if (start < m->base) start = m->base;
    if (end > m->base + m->len) end = m->base + m->len;
    start &= ~(uintptr_t)(MAP_PAGE - 1);
    for (uintptr_t a = start; a < end; a += MAP_PAGE)
        if (page_bit(m->commit, (a - m->base) / MAP_PAGE)) memset((void *)a, 0, MAP_PAGE);
}

static void lock(void);
static void unlock(void);
static mapping *find_mapping(uintptr_t a);

static void set_access(mapping *m, uintptr_t start, uintptr_t end, int on) {
    if (start < m->base) start = m->base;
    if (end > m->base + m->len) end = m->base + m->len;
    for (uintptr_t a = start & ~(uintptr_t)(MAP_PAGE - 1); a < end; a += MAP_PAGE) {
        size_t i = (a - m->base) / MAP_PAGE;
        if (on) m->access[i / 8] |= 1 << (i % 8);
        else m->access[i / 8] &= ~(1 << (i % 8));
    }
}

/*
 * Memory on first touch, as Linux does. Unity makes whole reservations
 * accessible at once (73 MB of 91 MB reserved by its first frame) and then
 * uses only parts of them; giving every accessible page memory right away
 * used up the pool before the game started. Accessible pages now get
 * memory when they are first read or written: the data abort comes here
 * (na_dynlib.c's abort handler), 64 KB around the address get memory, and
 * the instruction runs again.
 */
// 32 KB a touch: with 64 KB, nearly all of the pool's 160 MB had been handed
// out by 2400 touches when the game ran out of it. (Not less: every commit
// makes page objects in the kernel, and how many it has is unknown.)
#define FAULT_CHUNK (32 * 1024)
static int faults = 0;
volatile int na_exiting = 0; // set by na_mmap_teardown: no more memory for anyone

int na_mmap_fault(uint32_t addr) {
    if (na_exiting) return 0;
    lock();
    mapping *m = find_mapping(addr);
    int ok = 0;
    if (m && m->uid > 0) {
        size_t p = (addr - m->base) / MAP_PAGE;
        if (page_bit(m->access, p) && !page_bit(m->commit, p)) {
            uintptr_t start = addr & ~(uintptr_t)(FAULT_CHUNK - 1), end = start + FAULT_CHUNK;
            ok = commit_range(m, start, end, 1, 1) && page_bit(m->commit, p);
            faults++;
        } else if (page_bit(m->commit, p)) {
            ok = 1; // another thread got there first
        }
    }
    unlock();
    return ok;
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
    SceUInt quick = 2000; // usually free at once; only a long wait is noted
    if (sceKernelLockLwMutex(&map_lock, 1, &quick) >= 0) return;
    void na_thread_wait_begin(const char *what, const void *addr, int val);
    void na_thread_wait_end(void);
    na_thread_wait_begin("mmap lock", NULL, 0); // for the hang report
    sceKernelLockLwMutex(&map_lock, 1, NULL);
    na_thread_wait_end();
}

static void unlock(void) {
    sceKernelUnlockLwMutex(&map_lock, 1);
}

static mapping *find_mapping(uintptr_t a) {
    for (int i = 0; i < MAX_MAPPINGS; i++)
        if (mappings[i].base && !mappings[i].dead && a >= mappings[i].base && a < mappings[i].base + mappings[i].len)
            return &mappings[i];
    return NULL;
}

/*
 * Reservations that Unity unmapped keep their memory (the kernel can't be
 * asked to take it back, see decommit_range). Reused only for a new
 * reservation of exactly the same size, they held most of the pool while
 * the menu loaded (Unity frees a scene's blocks and asks for others), and
 * the pool ran out. A new reservation now borrows a free range of a dead
 * one: the addresses and the pages already backed with memory. When it is
 * unmapped in turn, the range goes back to the dead reservation.
 */
#define LEND_ALIGN (1024 * 1024)
static size_t lent_bytes = 0;

static void *lend_from_dead(size_t len, int slot) {
    if (slot < 0) return NULL;
    size_t pages = len / MAP_PAGE, step = LEND_ALIGN / MAP_PAGE;
    mapping *best = NULL;
    size_t best_off = 0, best_committed = 0;
    for (int i = 0; i < MAX_MAPPINGS; i++) {
        mapping *d = &mappings[i];
        if (!d->base || !d->dead || d->len < len || d->parent || d->arena) continue;
        size_t dp = d->len / MAP_PAGE;
        size_t first = ((LEND_ALIGN - (d->base % LEND_ALIGN)) % LEND_ALIGN) / MAP_PAGE;
        for (size_t off = first; off + pages <= dp; off += step) {
            size_t committed = 0;
            int free_run = 1;
            for (size_t k = 0; k < pages; k++) {
                if (d->lent && page_bit(d->lent, off + k)) {
                    free_run = 0;
                    break;
                }
                committed += page_bit(d->commit, off + k);
            }
            if (free_run && (!best || committed > best_committed)) {
                best = d;
                best_off = off;
                best_committed = committed;
            }
        }
    }
    if (!best || !best_committed) return NULL; // nothing to gain over a new reservation
    if (!best->lent && !(best->lent = calloc(1, (best->len / MAP_PAGE + 7) / 8))) return NULL;
    mapping *c = &mappings[slot];
    memset(c, 0, sizeof(*c));
    c->commit = calloc(1, (pages + 7) / 8);
    c->pool = calloc(pages, sizeof(uint32_t));
    c->access = calloc(1, (pages + 7) / 8);
    c->gone = calloc(1, (pages + 7) / 8);
    if (!c->commit || !c->pool || !c->access || !c->gone) {
        free(c->commit);
        free(c->pool);
        free(c->access);
        free(c->gone);
        memset(c, 0, sizeof(*c));
        return NULL;
    }
    for (size_t k = 0; k < pages; k++) {
        size_t q = best_off + k;
        best->lent[q / 8] |= 1 << (q % 8);
        if (page_bit(best->commit, q)) {
            c->commit[k / 8] |= 1 << (k % 8);
            c->pool[k] = best->pool[q];
        }
    }
    best->n_lent += pages;
    c->base = best->base + best_off * MAP_PAGE;
    c->len = len;
    c->uid = best->uid;
    c->parent = best;
    c->parent_page = best_off;
    lent_bytes += len;
    static int logged = 0;
    if (logged++ < 32)
        l_info("mmap: %u KB reserved at %p (lent by the unmapped reservation at %p, %u KB with memory)",
               (unsigned)(len / 1024), (void *)c->base, (void *)best->base, (unsigned)(best_committed * MAP_PAGE / 1024));
    return (void *)c->base;
}

// A borrowed range goes back (with whatever memory its pages have now)
static void give_back(mapping *c) {
    mapping *p = c->parent;
    size_t pages = c->len / MAP_PAGE;
    for (size_t k = 0; k < pages; k++) {
        size_t q = c->parent_page + k;
        p->lent[q / 8] &= ~(1 << (q % 8));
        if (page_bit(c->commit, k)) {
            p->commit[q / 8] |= 1 << (q % 8);
            p->pool[q] = c->pool[k];
        } else {
            p->commit[q / 8] &= ~(1 << (q % 8));
        }
    }
    p->n_lent -= pages;
    if (!p->arena) lent_bytes -= c->len;
    free(c->commit);
    free(c->pool);
    free(c->access);
    free(c->gone);
    memset(c, 0, sizeof(*c));
}

/*
 * Small anonymous mappings. Unity maps many of them (64 KB up to under
 * 1 MB, 38 MB in all by the intro scene); backed by malloc, they filled the
 * newlib heap, which vitaGL also falls back on for textures, while the
 * pool still had 67 MB. They now come from one large reservation, the
 * arena, page by page (first fit), and get memory from the pool on first
 * touch like the reservations. Unmapped, their pages go back to the arena
 * with their memory, zeroed, for the next one.
 */
#define ARENA_SIZE (96u * 1024 * 1024)
#define ARENA_MIN (16 * 1024)
static mapping *arena = NULL;
static size_t arena_hint = 0;

static void *arena_map(size_t len, int prot, int slot) {
    if (slot < 0) return NULL;
    if (!arena) {
        static int failed = 0;
        if (failed) return NULL;
        size_t pages = ARENA_SIZE / MAP_PAGE;
        void *base = NULL;
        uint8_t *commit = calloc(1, (pages + 7) / 8), *acc = calloc(1, (pages + 7) / 8), *gone = calloc(1, (pages + 7) / 8),
                *lent = calloc(1, (pages + 7) / 8);
        uint32_t *pidx = calloc(pages, sizeof(uint32_t));
        SceUID uid = commit && acc && gone && lent && pidx && pool_init()
                         ? kuKernelMemReserve(&base, ARENA_SIZE, SCE_KERNEL_MEMBLOCK_TYPE_USER_RW)
                         : -1;
        if (uid <= 0 || !base) {
            free(commit), free(acc), free(gone), free(lent), free(pidx);
            failed = 1;
            l_warn("mmap: no arena for small mappings (0x%08x)", (unsigned)uid);
            return NULL;
        }
        mapping *a = &mappings[slot];
        memset(a, 0, sizeof(*a));
        a->base = (uintptr_t)base;
        a->len = ARENA_SIZE;
        a->uid = uid;
        a->commit = commit;
        a->access = acc;
        a->gone = gone;
        a->lent = lent;
        a->pool = pidx;
        a->dead = 1; // never found by address: its borrowers are
        a->arena = 1;
        arena = a;
        reserved_bytes += ARENA_SIZE;
        void na_faults_enable(void); // na_dynlib.c
        na_faults_enable();
        l_info("mmap: arena of %u MB for small mappings at %p", (unsigned)(ARENA_SIZE >> 20), base);
        // the arena took this slot: find another for the mapping
        slot = -1;
        for (int i = 0; i < MAX_MAPPINGS; i++)
            if (!mappings[i].base) {
                slot = i;
                break;
            }
        if (slot < 0) return NULL;
    }
    size_t pages = len / MAP_PAGE, total = ARENA_SIZE / MAP_PAGE, off = (size_t)-1;
    for (size_t k = 0, run = 0; k < total; k++) {
        size_t q = (arena_hint + k) % total;
        if (q == 0) run = 0; // runs don't wrap around
        if (page_bit(arena->lent, q)) {
            run = 0;
            continue;
        }
        if (++run == pages) {
            off = q + 1 - pages;
            break;
        }
    }
    if (off == (size_t)-1) {
        static int warned = 0;
        if (warned++ < 4) l_warn("mmap: arena full (%u KB asked)", (unsigned)(len / 1024));
        return NULL;
    }
    mapping *c = &mappings[slot];
    memset(c, 0, sizeof(*c));
    c->commit = calloc(1, (pages + 7) / 8);
    c->pool = calloc(pages, sizeof(uint32_t));
    c->access = calloc(1, (pages + 7) / 8);
    c->gone = calloc(1, (pages + 7) / 8);
    if (!c->commit || !c->pool || !c->access || !c->gone) {
        free(c->commit), free(c->pool), free(c->access), free(c->gone);
        memset(c, 0, sizeof(*c));
        return NULL;
    }
    for (size_t k = 0; k < pages; k++) {
        size_t q = off + k;
        arena->lent[q / 8] |= 1 << (q % 8);
        if (page_bit(arena->commit, q)) {
            c->commit[k / 8] |= 1 << (k % 8);
            c->pool[k] = arena->pool[q];
        }
        if (prot) c->access[k / 8] |= 1 << (k % 8);
    }
    arena->n_lent += pages;
    arena_hint = off + pages;
    c->base = arena->base + off * MAP_PAGE;
    c->len = len;
    c->uid = arena->uid;
    c->parent = arena;
    c->parent_page = off;
    return (void *)c->base;
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
                // a fresh mapping: the old pages go; anonymous ones get memory on first touch
                decommit_range(m, (uintptr_t)addr, (uintptr_t)addr + len);
                set_access(m, (uintptr_t)addr, (uintptr_t)addr + len, prot != 0);
                if (fd >= 0 && !commit_range(m, (uintptr_t)addr, (uintptr_t)addr + len, 1, 0)) {
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
        if (prot == 0 && fd < 0 && len >= RESERVE_MIN) {
            // an earlier reservation of this size that was unmapped: its
            // address space and memory again (pages zeroed, none accessible)
            for (int i = 0; i < MAX_MAPPINGS; i++) {
                mapping *d = &mappings[i];
                if (!d->base || !d->dead || d->len != len || d->n_lent || d->arena) continue;
                size_t pages = len / MAP_PAGE;
                memset(d->gone, 0, (pages + 7) / 8);
                memset(d->access, 0, (pages + 7) / 8);
                d->n_gone = 0;
                d->dead = 0;
                unlock();
                static int logged_u = 0;
                if (logged_u++ < 24) l_info("mmap: %u KB reserved at %p (reused)", (unsigned)(len / 1024), (void *)d->base);
                return (void *)d->base;
            }
            void *lent = lend_from_dead(len, slot);
            if (lent) {
                void na_faults_enable(void); // na_dynlib.c
                na_faults_enable();
                unlock();
                return lent;
            }
        }
        if (slot >= 0 && prot == 0 && fd < 0 && len >= RESERVE_MIN) {
            // address space only
            uint8_t *commit = calloc(1, (len / MAP_PAGE + 7) / 8);
            uint32_t *pidx = calloc(len / MAP_PAGE, sizeof(uint32_t));
            uint8_t *acc = calloc(1, (len / MAP_PAGE + 7) / 8);
            gone = calloc(1, (len / MAP_PAGE + 7) / 8);
            void *base = NULL;
            SceUID uid = commit && gone && pidx && acc && pool_init() ? kuKernelMemReserve(&base, len, SCE_KERNEL_MEMBLOCK_TYPE_USER_RW) : -1;
            if (uid > 0 && base) {
                mappings[slot].base = (uintptr_t)base;
                mappings[slot].len = len;
                mappings[slot].gone = gone;
                mappings[slot].n_gone = 0;
                mappings[slot].uid = uid;
                mappings[slot].commit = commit;
                mappings[slot].pool = pidx;
                mappings[slot].access = acc;
                mappings[slot].dead = 0;
                reserved_bytes += len;
                void na_faults_enable(void); // na_dynlib.c
                na_faults_enable();
                unlock();
                static int logged_r = 0;
                if (logged_r++ < 48) l_info("mmap: %u KB reserved at %p (no memory yet)", (unsigned)(len / 1024), base);
                return base;
            }
            static int warned = 0;
            if (warned++ < 4) l_warn("mmap: kuKernelMemReserve(%u KB) failed (0x%x): taken from the heap", (unsigned)(len / 1024), uid);
            free(commit);
            free(pidx);
            free(acc);
            free(gone);
            gone = NULL;
        }
        if (slot >= 0 && fd < 0 && len >= ARENA_MIN && len < RESERVE_MIN * 4) {
            void *a = arena_map(len, prot, slot);
            if (a) {
                unlock();
                static int logged_a = 0;
                if (logged_a++ < 8) l_info("mmap: %u KB from the arena at %p", (unsigned)(len / 1024), a);
                return a;
            }
            // the arena may have taken the slot
            slot = -1;
            for (int i = 0; i < MAX_MAPPINGS; i++)
                if (!mappings[i].base) {
                    slot = i;
                    break;
                }
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
        plain_mmap_bytes += len;
        mappings[slot].base = (uintptr_t)ret;
        mappings[slot].len = len;
        mappings[slot].gone = gone;
        mappings[slot].n_gone = 0;
        mappings[slot].uid = 0;
        mappings[slot].commit = NULL;
        mappings[slot].pool = NULL;
        mappings[slot].access = NULL;
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
        munmap_calls++;
        munmap_bytes += length;
        // pages are counted once even if unmapped repeatedly; the memory is
        // freed when every page of the mapping is unmapped
        mark_pages(m, (uintptr_t)addr, (uintptr_t)addr + length, 1);
        if (m->uid > 0) {
            decommit_range(m, (uintptr_t)addr, (uintptr_t)addr + length);
            set_access(m, (uintptr_t)addr, (uintptr_t)addr + length, 0);
        }
        if (m->n_gone * MAP_PAGE >= m->len) {
            if (m->parent) {
                give_back(m);
                unlock();
                return 0;
            }
            if (m->uid > 0) {
                // kept with its memory (zeroed above) for the next
                // reservation of the same size: freeing a block with
                // committed pages is what the kernel didn't take
                m->dead = 1;
                unlock();
                return 0;
            } else {
                plain_mmap_bytes -= m->len;
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
    if (!length) return 0;
    if (!prot) {
        lock();
        mapping *m = find_mapping((uintptr_t)addr);
        if (m && m->uid > 0) set_access(m, (uintptr_t)addr, (uintptr_t)addr + length, 0);
        unlock();
        return 0;
    }
    int ok = 1;
    lock();
    mapping *m = find_mapping((uintptr_t)addr);
    if (m && m->uid > 0) set_access(m, (uintptr_t)addr, (uintptr_t)addr + length, 1);
    unlock();
    if (!ok) {
        errno = ENOMEM;
        return -1;
    }
    return 0;
}

// madvise(MADV_DONTNEED = 4): the pages lose their memory and read as zero next time
int na_madvise(void *addr, size_t length, int advice) {
    if (advice != 4) return 0;
    lock();
    madvise_calls++;
    madvise_bytes += length;
    // MADV_DONTNEED: the pages read as zero afterwards. They keep their
    // memory (no kernel call here: Unity's madvise was where the main thread
    // hung in kuKernelMemDecommit); munmap gives it back.
    mapping *m = find_mapping((uintptr_t)addr);
    if (m && m->uid > 0) {
        uintptr_t start = (uintptr_t)addr, end = start + length;
        if (start < m->base) start = m->base;
        if (end > m->base + m->len) end = m->base + m->len;
        start &= ~(uintptr_t)(MAP_PAGE - 1);
        for (uintptr_t a = start; a < end; a += MAP_PAGE)
            if (page_bit(m->commit, (a - m->base) / MAP_PAGE)) memset((void *)a, 0, MAP_PAGE);
    }
    unlock();
    return 0;
}

int na_mmap_fault_count(void) { return faults; }

/*
 * Buffers handed to the kernel. A file read into a reserved page that was
 * never touched does not fault: the kernel just fails the read (Unity's
 * first read of the APK into such a buffer failed, and it could not start).
 * read/pread/fread give such buffers their memory first.
 */
void na_mmap_touch(const void *buf, size_t len) {
    if (!buf || !len || !reserved_bytes) return;
    uintptr_t a = (uintptr_t)buf, end = a + len;
    lock();
    while (a < end) {
        mapping *m = find_mapping(a);
        if (!m) {
            a = (a & ~(uintptr_t)(MAP_PAGE - 1)) + MAP_PAGE;
            continue;
        }
        uintptr_t e = m->base + m->len < end ? m->base + m->len : end;
        if (m->uid > 0) commit_range(m, a, e, 0, 1);
        a = e;
    }
    unlock();
}

// More for the memory log: what is left of the pool, and how much memory
// sits in pages the game gave up (unmapped or protected again after
// madvise(DONTNEED)): it stays where it is (see decommit_range).
void na_mmap_report2(size_t *pool_total_kb, size_t *pool_free_kb, size_t *idle_kb, size_t *stats) {
    size_t idle = 0;
    for (int i = 0; i < MAX_MAPPINGS; i++) {
        mapping *m = &mappings[i];
        if (!m->base || m->uid <= 0 || !m->commit) continue;
        size_t pages = m->len / MAP_PAGE;
        for (size_t p = 0; p < pages; p++) {
            if (m->lent && page_bit(m->lent, p)) continue; // counted in the borrower
            if (page_bit(m->commit, p) && (m->dead || !page_bit(m->access, p))) idle++;
        }
    }
    *pool_total_kb = (pool_pages - pool_reserved_pages) * (MAP_PAGE / 1024) + plain_pages * (MAP_PAGE / 1024);
    *pool_free_kb = pool_free * (MAP_PAGE / 1024);
    *idle_kb = idle * (MAP_PAGE / 1024);
    stats[0] = madvise_calls;
    stats[1] = madvise_bytes / 1024;
    stats[2] = munmap_calls;
    stats[3] = munmap_bytes / 1024;
    stats[4] = plain_mmap_bytes / 1024;
}

/*
 * Leaving in order. The pages of the reservations are mapped from the pool's
 * blocks (kubridge "commit with a base block"); the kernel does not know that
 * a reservation depends on a pool block. When the process is simply ended,
 * everything is torn down in the kernel's own order, and the console was
 * left unstable afterwards (frozen LiveArea, apps not starting). Here the
 * reservations are freed first, while the blocks behind their pages still
 * exist, then the pool, and only then does the process end (main.c).
 *
 * Other threads may still run: from now on a first touch gets no memory
 * (na_mmap_fault) and the fault handler parks the thread (na_dynlib.c). The
 * lock is taken and kept, so a commit under way finishes first.
 */
void log_flush(void);
void log_sync(void);
void na_mmap_teardown(void) {
    na_exiting = 1;
    lock();
    int n = 0, failed = 0;
    l_info("mmap: leaving: freeing the reservations (%u KB reserved, %u KB with memory)",
           (unsigned)(reserved_bytes / 1024), (unsigned)(committed_bytes / 1024));
    log_flush();
    log_sync();
    for (int i = 0; i < MAX_MAPPINGS; i++) {
        mapping *m = &mappings[i];
        if (!m->base || m->uid <= 0 || m->parent) continue;
        int r = sceKernelFreeMemBlock(m->uid);
        if (r < 0) failed++;
        n++;
        m->uid = 0;
    }
    if (trial_uid > 0) {
        if (sceKernelFreeMemBlock(trial_uid) < 0) failed++;
        n++;
    }
    l_info("mmap: leaving: %d reservations freed (%d refused); freeing the pool's %d blocks", n, failed, pool_blocks);
    log_flush();
    log_sync();
    failed = 0;
    for (int i = 0; i < pool_blocks; i++)
        if (sceKernelFreeMemBlock(pool_uids[i]) < 0) failed++;
    l_info("mmap: leaving: pool freed (%d refused)", failed);
    log_flush();
    log_sync();
}
