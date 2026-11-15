/*
 * Import profile of a hung thread.
 *
 * User code can't read another thread's registers, and a busy thread's
 * stack is full of stale return addresses. But a loop in Unity or IL2CPP
 * nearly always calls into the loader (malloc, locks, clocks, memcpy...),
 * and every such call goes through the libraries' GOT. For a short while
 * each PLT slot of libunity and libil2cpp is pointed at a small trampoline
 * (in the library's patch area) that counts the call with its caller (lr),
 * for the watched thread only, and then jumps on to the real function with
 * all registers and the stack as they were. Afterwards the slots get their
 * old values back and the busiest (import, caller) pairs go to the log.
 * Only data (the GOT) changes: no memory protection is touched.
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <psp2/kernel/threadmgr.h>
#include <kubridge.h>

#include "na_modules.h"
#include "utils/logger.h"

#define MAX_SLOTS 4096
#define HIST 1024

static uintptr_t orig[MAX_SLOTS];
static uintptr_t *slot_ptr[MAX_SLOTS];
static const char *slot_name[MAX_SLOTS];
static uint32_t *tramp[MAX_SLOTS];
static int n_slots = 0;

static volatile SceUID watch_tid = -1;
static volatile uint32_t calls_watched = 0, calls_other = 0;
static struct {
    uint32_t key_lr;
    uint16_t idx;
    uint32_t count;
} hist[HIST];

// Called by the trampolines (r0 = slot index, r1 = caller's lr); returns
// the real function.
uintptr_t na_gotprof_record(uint32_t idx, uint32_t lr) {
    if (sceKernelGetThreadId() == watch_tid) {
        calls_watched++;
        uint32_t h = ((lr >> 1) * 2654435761u ^ idx * 40503u) % HIST;
        for (int n = 0; n < 16; n++) {
            int k = (h + n) % HIST;
            if (hist[k].count == 0) {
                hist[k].key_lr = lr;
                hist[k].idx = (uint16_t)idx;
                hist[k].count = 1;
                break;
            }
            if (hist[k].key_lr == lr && hist[k].idx == idx) {
                hist[k].count++;
                break;
            }
        }
    } else {
        calls_other++;
    }
    return orig[idx];
}

// ARM: saves the argument registers, records, jumps on (r12 = slot index)
__attribute__((naked, target("arm"))) void na_gotprof_common(void) {
    __asm__ volatile(
        "push {r0-r5, r12, lr}\n"
        "mov r0, r12\n"
        "mov r1, lr\n"
        "bl na_gotprof_record\n"
        "str r0, [sp, #24]\n" // becomes r12: the real function
        "pop {r0-r5, r12, lr}\n"
        "bx r12\n");
}

static const char *fmt(uint32_t v, char *buf, size_t n) {
    if (v >= 0x9A400000u && v < 0x9A400000u + na_libs[NA_LIB_UNITY].mod.text_size)
        snprintf(buf, n, "U+%x", (unsigned)(v - 0x9A400000u));
    else if (v >= 0x98100000u && v < 0x98100000u + na_libs[NA_LIB_IL2CPP].mod.text_size)
        snprintf(buf, n, "I+%x", (unsigned)(v - 0x98100000u));
    else
        snprintf(buf, n, "%x", (unsigned)v);
    return buf;
}

static int install(void) {
    n_slots = 0;
    for (int l = 0; l < NA_LIB_COUNT; l++) {
        NaLib *lib = &na_libs[l];
        if (!lib->loaded) continue;
        uint32_t *code = (uint32_t *)((lib->mod.patch_head + 7) & ~7u);
        uint32_t *end = (uint32_t *)(lib->mod.patch_base + lib->mod.patch_size);
        uint32_t *start = code;
        for (uint32_t i = 0; i < lib->mod.num_relplt && n_slots < MAX_SLOTS; i++) {
            Elf32_Rel *rel = &lib->mod.relplt[i];
            if (ELF32_R_TYPE(rel->r_info) != R_ARM_JUMP_SLOT) continue;
            if (code + 3 > end) break;
            Elf32_Sym *sym = &lib->mod.dynsym[ELF32_R_SYM(rel->r_info)];
            int idx = n_slots++;
            slot_ptr[idx] = (uintptr_t *)(lib->mod.load_addr + rel->r_offset);
            slot_name[idx] = lib->mod.dynstr + sym->st_name;
            orig[idx] = *slot_ptr[idx];
            tramp[idx] = code;
            code[0] = 0xE300C000u | ((idx >> 12) << 16) | (idx & 0xfff); // movw r12, #idx
            code[1] = 0xE51FF004u;                                        // ldr pc, [pc, #-4]
            code[2] = (uint32_t)(uintptr_t)&na_gotprof_common;
            code += 3;
        }
        kuKernelFlushCaches(start, (uintptr_t)code - (uintptr_t)start);
    }
    // all trampolines ready: switch the slots
    for (int i = 0; i < n_slots; i++) *slot_ptr[i] = (uintptr_t)tramp[i];
    return n_slots;
}

static void uninstall(void) {
    for (int i = 0; i < n_slots; i++) *slot_ptr[i] = orig[i];
}

void na_gotprof_run(SceUID tid, int ms) {
    memset(hist, 0, sizeof(hist));
    calls_watched = calls_other = 0;
    watch_tid = tid;
    int n = install();
    sceKernelDelayThread(ms * 1000);
    uninstall();
    watch_tid = -1;
    sceKernelDelayThread(20 * 1000); // calls already in a trampoline finish
    l_info("[imports] %d slots watched; in %d ms the thread made %u calls (others %u)", n, ms,
           (unsigned)calls_watched, (unsigned)calls_other);
    char a[24];
    for (int top = 0; top < 30; top++) {
        int best = -1;
        for (int i = 0; i < HIST; i++)
            if (hist[i].count && (best < 0 || hist[i].count > hist[best].count)) best = i;
        if (best < 0) break;
        l_info("[imports] %6u x %-28s from %s", (unsigned)hist[best].count, slot_name[hist[best].idx],
               fmt(hist[best].key_lr, a, sizeof(a)));
        hist[best].count = 0;
    }
}
