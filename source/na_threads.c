/*
 * Thread report for hangs.
 *
 * The Vita's core dumps of this port often stop at 0%, so the watchdog writes
 * what every thread is doing to the log itself:
 *  - every game thread registers itself when it starts (pthread trampoline);
 *  - futex waits (Unity's and IL2CPP's locks, job fences) record the futex
 *    and the code addresses found on the waiting thread's stack;
 *  - the report reads each thread's state and CPU time from the kernel twice,
 *    so busy threads show up, and lists code addresses on their stacks.
 * Addresses are printed as U+offset (libunity), I+offset (libil2cpp) and
 * L+offset (the loader).
 */

#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/kernel/processmgr.h>

#include "na_threads.h"
#include "utils/logger.h"

#define MAX_THREADS 128
#define CHAIN 16

#define IL2CPP_LO 0x98100000u
#define IL2CPP_HI (0x98100000u + 0x1ec5194u)
#define UNITY_LO 0x9A400000u
#define UNITY_HI (0x9A400000u + 0x1036d8cu)
#define LOADER_LO 0x81000000u
#define LOADER_HI 0x81200000u

typedef struct {
    volatile SceUID tid;
    void *start;
    char tag[16];
    uintptr_t stack_lo, stack_hi;
    // current wait (written by the thread itself)
    volatile int waiting;
    const char *wait_what;
    uintptr_t wait_addr;
    int wait_val;
    uint64_t wait_since;
    uint32_t chain[CHAIN];
    int chain_n;
} thr_slot;

static thr_slot slots[MAX_THREADS];
static volatile int n_slots = 0;

static int is_code(uint32_t v) {
    return (v >= UNITY_LO && v < UNITY_HI) || (v >= IL2CPP_LO && v < IL2CPP_HI) ||
           (v >= LOADER_LO && v < LOADER_HI);
}

static const char *fmt_addr(uint32_t v, char *buf, size_t n) {
    if (v >= UNITY_LO && v < UNITY_HI) snprintf(buf, n, "U+%x", (unsigned)(v - UNITY_LO));
    else if (v >= IL2CPP_LO && v < IL2CPP_HI) snprintf(buf, n, "I+%x", (unsigned)(v - IL2CPP_LO));
    else if (v >= LOADER_LO && v < LOADER_HI) snprintf(buf, n, "L+%x", (unsigned)(v - LOADER_LO));
    else snprintf(buf, n, "%x", (unsigned)v);
    return buf;
}

static thr_slot *find_slot(SceUID tid) {
    int n = n_slots;
    for (int i = 0; i < n && i < MAX_THREADS; i++)
        if (slots[i].tid == tid) return &slots[i];
    return NULL;
}

void na_thread_register(const char *tag, void *start) {
    SceUID tid = sceKernelGetThreadId();
    if (find_slot(tid)) return;
    int i = __sync_fetch_and_add(&n_slots, 1);
    if (i >= MAX_THREADS) return;
    thr_slot *s = &slots[i];
    s->start = start;
    snprintf(s->tag, sizeof(s->tag), "%s", tag ? tag : "");
    SceKernelThreadInfo info;
    memset(&info, 0, sizeof(info));
    info.size = sizeof(info);
    if (sceKernelGetThreadInfo(tid, &info) >= 0) {
        s->stack_lo = (uintptr_t)info.stack;
        s->stack_hi = (uintptr_t)info.stack + info.stackSize;
    }
    __sync_synchronize();
    s->tid = tid;
}

static int scan_stack(uintptr_t from, uintptr_t to, uint32_t *out, int max) {
    int n = 0;
    from &= ~3u;
    for (uintptr_t p = from; p + 4 <= to && n < max; p += 4) {
        uint32_t v = *(volatile uint32_t *)p;
        if (is_code(v)) out[n++] = v;
    }
    return n;
}

void na_thread_wait_begin(const char *what, const void *addr, int val) {
    thr_slot *s = find_slot(sceKernelGetThreadId());
    if (!s) return;
    s->wait_what = what;
    s->wait_addr = (uintptr_t)addr;
    s->wait_val = val;
    s->wait_since = sceKernelGetProcessTimeWide();
    uintptr_t sp = (uintptr_t)__builtin_frame_address(0);
    uintptr_t top = s->stack_hi && sp < s->stack_hi ? s->stack_hi : sp + 4096;
    if (top > sp + 4096) top = sp + 4096;
    s->chain_n = scan_stack(sp, top, s->chain, CHAIN);
    __sync_synchronize();
    s->waiting = 1;
}

void na_thread_wait_end(void) {
    thr_slot *s = find_slot(sceKernelGetThreadId());
    if (s) s->waiting = 0;
}

static const char *status_name(int st) {
    switch (st) {
        case 1: return "RUN";
        case 2: return "READY";
        case 4: return "STANDBY";
        case 8: return "WAIT";
        case 16: return "DORMANT";
        case 32: return "DELETED";
        case 128: return "STAGNANT";
        case 256: return "SUSP";
        default: return "?";
    }
}

void na_threads_report(void) {
    int n = n_slots;
    if (n > MAX_THREADS) n = MAX_THREADS;
    static uint64_t clocks0[MAX_THREADS];
    SceKernelThreadInfo info;
    for (int i = 0; i < n; i++) {
        memset(&info, 0, sizeof(info));
        info.size = sizeof(info);
        clocks0[i] = sceKernelGetThreadInfo(slots[i].tid, &info) >= 0 ? (uint64_t)info.runClocks : 0;
    }
    sceKernelDelayThread(500 * 1000);
    uint64_t now = sceKernelGetProcessTimeWide();
    l_info("[THREADS] %d threads (CPU = microseconds run in the last 0.5 s)", n);
    char line[512], a[24];
    for (int i = 0; i < n; i++) {
        thr_slot *s = &slots[i];
        memset(&info, 0, sizeof(info));
        info.size = sizeof(info);
        if (sceKernelGetThreadInfo(s->tid, &info) < 0) continue;
        if (info.status == 16 || info.status == 32) continue;
        unsigned cpu = (unsigned)((uint64_t)info.runClocks - clocks0[i]);
        int len = snprintf(line, sizeof(line), "[THREADS] %-10s %s start %s %s CPU %u core %d",
                           info.name, s->tag, fmt_addr((uint32_t)(uintptr_t)s->start, a, sizeof(a)),
                           status_name(info.status), cpu, info.currentCpuId);
        if (s->waiting) {
            len += snprintf(line + len, sizeof(line) - len, " | %s %x (=%d, now %d) %u ms:",
                            s->wait_what ? s->wait_what : "wait", (unsigned)s->wait_addr, s->wait_val,
                            s->wait_addr ? *(volatile int *)s->wait_addr : 0,
                            (unsigned)((now - s->wait_since) / 1000));
            for (int k = 0; k < s->chain_n && len < (int)sizeof(line) - 16; k++)
                len += snprintf(line + len, sizeof(line) - len, " %s", fmt_addr(s->chain[k], a, sizeof(a)));
        } else if (s->stack_lo) {
            // not in a futex: code addresses on its stack, outermost first
            uint32_t found[64];
            int m = 0;
            for (uintptr_t p = s->stack_hi - 4; p >= s->stack_lo && m < 64; p -= 4) {
                uint32_t v = *(volatile uint32_t *)p;
                if (is_code(v)) found[m++] = v;
            }
            len += snprintf(line + len, sizeof(line) - len, " | stack:");
            for (int k = 0; k < m && len < (int)sizeof(line) - 16; k++)
                len += snprintf(line + len, sizeof(line) - len, " %s", fmt_addr(found[k], a, sizeof(a)));
        }
        l_info("%s", line);
    }
}
