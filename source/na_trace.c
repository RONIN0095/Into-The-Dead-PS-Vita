/*
 * Hang tracing: the main thread's last calls into the loader.
 *
 * In Into the Dead's menu the main thread hung at full speed while making
 * no calls through the libraries' PLT and running no Unity or IL2CPP code:
 * it was stuck inside a loader function it had entered earlier. To see
 * which one, every way Unity and IL2CPP call into the loader goes through a
 * small trampoline for the whole run: their PLT slots (GOT), the GL entry
 * points dlsym hands out, and the JNI function table. The trampoline notes
 * (function, caller) in a ring when the caller runs on the main thread's
 * stack, then jumps on to the real function with the registers and the
 * stack as they were. The watchdog's report prints the ring: its newest
 * entries are where the main thread went last.
 *
 * Only pointers (data) change; the trampolines live in a block of their own.
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <psp2/kernel/sysmem.h>
#include <psp2/kernel/threadmgr.h>
#include <kubridge.h>
#include <falso_jni/FalsoJNI.h>

#include "na_modules.h"
#include "na_trace.h"
#include "utils/logger.h"

extern struct JNINativeInterface *_jni; // FalsoJNI
volatile int na_main_phase = 0;

#define MAX_FNS 4096
#define RING 32

static uintptr_t fn_target[MAX_FNS];
static const char *fn_name[MAX_FNS];
static int n_fns = 0;
static uint32_t *code_next = NULL, *code_end = NULL;

static uintptr_t main_lo = 0, main_hi = 0;
static volatile uint32_t ring_idx[RING], ring_lr[RING];
static volatile uint32_t ring_pos = 0;

// r0 = function index, r1 = caller's lr, r2 = caller's sp; returns the real function
uintptr_t na_trace_record(uint32_t idx, uint32_t lr, uintptr_t sp) {
    if (sp - main_lo < main_hi - main_lo) {
        uint32_t p = ring_pos++ % RING;
        ring_idx[p] = idx;
        ring_lr[p] = lr;
    }
    return fn_target[idx];
}

__attribute__((naked, target("arm"))) void na_trace_common(void) {
    __asm__ volatile(
        "push {r0-r5, r12, lr}\n"
        "mov r0, r12\n"
        "mov r1, lr\n"
        "add r2, sp, #32\n"
        "bl na_trace_record\n"
        "str r0, [sp, #24]\n" // r12 := the real function
        "pop {r0-r5, r12, lr}\n"
        "bx r12\n");
}

// The trampolines go into the libraries' patch areas (64 KB each, already
// readable, writable and executable; so_util allocates them below each
// library and the port doesn't use them otherwise).
static int region = 0;
static int block_init(void) {
    while (region < NA_LIB_COUNT) {
        NaLib *lib = &na_libs[region];
        if (!code_next && lib->loaded && lib->mod.patch_size) {
            code_next = (uint32_t *)((lib->mod.patch_head + 7) & ~7u);
            code_end = (uint32_t *)(lib->mod.patch_base + lib->mod.patch_size);
        }
        if (code_next && code_next + 3 <= code_end) return 1;
        code_next = NULL;
        region++;
    }
    return 0;
}

void *na_trace_wrap(const char *name, void *fn) {
    if (!fn || !block_init() || n_fns >= MAX_FNS) return fn;
    int idx = n_fns++;
    fn_target[idx] = (uintptr_t)fn;
    fn_name[idx] = name;
    uint32_t *c = code_next;
    c[0] = 0xE300C000u | ((idx >> 12) << 16) | (idx & 0xfff); // movw r12, #idx
    c[1] = 0xE51FF004u;                                        // ldr pc, [pc, #-4]
    c[2] = (uint32_t)(uintptr_t)&na_trace_common;
    code_next += 3;
    kuKernelFlushCaches(c, 12);
    return c;
}

static const char *jni_names[] = { "GetVersion", "DefineClass", "FindClass", "FromReflectedMethod",
                                   "FromReflectedField", "ToReflectedMethod", "GetSuperclass" };

void na_trace_start(void) {
    SceKernelThreadInfo ti;
    memset(&ti, 0, sizeof(ti));
    ti.size = sizeof(ti);
    if (sceKernelGetThreadInfo(sceKernelGetThreadId(), &ti) < 0) return;
    // PLT slots of both libraries
    int got = 0;
    for (int l = 0; l < NA_LIB_COUNT; l++) {
        NaLib *lib = &na_libs[l];
        if (!lib->loaded) continue;
        for (uint32_t i = 0; i < lib->mod.num_relplt; i++) {
            Elf32_Rel *rel = &lib->mod.relplt[i];
            if (ELF32_R_TYPE(rel->r_info) != R_ARM_JUMP_SLOT) continue;
            uintptr_t *slot = (uintptr_t *)(lib->mod.load_addr + rel->r_offset);
            void *w = na_trace_wrap(lib->mod.dynstr + lib->mod.dynsym[ELF32_R_SYM(rel->r_info)].st_name, (void *)*slot);
            if (w == (void *)*slot) continue;
            *slot = (uintptr_t)w;
            got++;
        }
    }
    // the JNI function table (every entry after the 4 reserved ones is a function)
    int jni = 0;
    if (_jni) {
        void **t = (void **)_jni;
        int n = sizeof(struct JNINativeInterface) / sizeof(void *);
        static char names[300][24];
        for (int i = 4; i < n && i - 4 < 300; i++) {
            if (!t[i]) continue;
            if (i - 4 < (int)(sizeof(jni_names) / sizeof(jni_names[0]))) snprintf(names[i - 4], 24, "JNI %s", jni_names[i - 4]);
            else snprintf(names[i - 4], 24, "JNI #%d", i);
            void *w = na_trace_wrap(names[i - 4], t[i]);
            if (w != t[i]) {
                t[i] = w;
                jni++;
            }
        }
    }
    main_lo = (uintptr_t)ti.stack;
    main_hi = (uintptr_t)ti.stack + ti.stackSize;
    l_info("[trace] main thread's calls into the loader recorded: %d PLT slots, %d JNI functions, %d in all", got, jni,
           n_fns);
}

static const char *fmt(uint32_t v, char *buf, size_t n) {
    if (v >= 0x9A400000u && v < 0x9A400000u + na_libs[NA_LIB_UNITY].mod.text_size)
        snprintf(buf, n, "U+%x", (unsigned)(v - 0x9A400000u));
    else if (v >= 0x98100000u && v < 0x98100000u + na_libs[NA_LIB_IL2CPP].mod.text_size)
        snprintf(buf, n, "I+%x", (unsigned)(v - 0x98100000u));
    else if (v >= 0x81000000u && v < 0x81200000u)
        snprintf(buf, n, "L+%x", (unsigned)(v - 0x81000000u));
    else
        snprintf(buf, n, "%x", (unsigned)v);
    return buf;
}

void na_trace_report(void) {
    uint32_t pos = ring_pos;
    l_info("[trace] main loop phase %d (1 Unity start, 2 input, 3 looper, 4 Java tick, 5 Unity frame, 6 file cache, 7 HUD); "
           "%u calls into the loader recorded, the last %d (newest last):",
           na_main_phase, (unsigned)pos, pos < RING ? (int)pos : RING);
    char a[24];
    for (uint32_t k = pos > RING ? pos - RING : 0; k < pos; k++) {
        uint32_t p = k % RING;
        uint32_t idx = ring_idx[p];
        l_info("[trace]   %s from %s", idx < (uint32_t)n_fns ? fn_name[idx] : "?", fmt(ring_lr[p], a, sizeof(a)));
    }
    // a second look: does the main thread still call anything?
    sceKernelDelayThread(1000 * 1000);
    l_info("[trace] one second later: %u calls recorded (%u new)", (unsigned)ring_pos, (unsigned)(ring_pos - pos));
}
