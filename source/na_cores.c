/*
 * Copyright (C) 2026 Vita port contributors
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

/**
 * @file  na_cores.c
 * @brief The fourth CPU core, when a plugin (CapUnlocker) frees it.
 *
 * Applications get cores 0-2; the fourth is the system's unless a plugin
 * opens it up. At startup a thread allowed only on core 3 is started: if it
 * runs within 200 ms, Unity is told about four cores (sysconf,
 * /sys/devices/system/cpu/present, sched_getaffinity) and its threads may
 * use all four. Otherwise everything stays at three.
 */

#include <psp2/kernel/threadmgr.h>
#include <psp2/io/fcntl.h>
#include <string.h>

#include "utils/logger.h"

#ifndef SCE_KERNEL_CPU_MASK_USER_3
#define SCE_KERNEL_CPU_MASK_USER_3 0x80000
#endif

int na_cores = 3;

#define CPU_PRESENT4 "ux0:data/intothedead/cpu_present4"

static volatile int probe_core = -1;

static int probe_thread(SceSize args, void *argp) {
    probe_core = 3; // it may only run on core 3: running at all is the answer
    return 0;
}

void na_cores_probe(void) {
    SceUID t = sceKernelCreateThread("core3probe", probe_thread, 0x10000100, 0x1000, 0,
                                     SCE_KERNEL_CPU_MASK_USER_3, NULL);
    if (t < 0) {
        l_info("Fourth CPU core: not available (0x%08x); 3 cores", t);
        return;
    }
    int r = sceKernelStartThread(t, 0, NULL);
    SceUInt timeout = 200000;
    int stat = 0;
    if (r >= 0 && sceKernelWaitThreadEnd(t, &stat, &timeout) >= 0 && probe_core == 3) {
        na_cores = 4;
        SceUID f = sceIoOpen(CPU_PRESENT4, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0777);
        if (f >= 0) {
            sceIoWrite(f, "0-3\n", 4);
            sceIoClose(f);
        } else {
            na_cores = 3;
        }
    }
    if (probe_core >= 0 || r < 0)
        sceKernelDeleteThread(t); // a thread that never ran stays parked, harmless
    l_info("Fourth CPU core: %s (probe ran on core %d); %d cores",
           na_cores == 4 ? "unlocked, used by the game" : "locked (CapUnlocker not installed)", probe_core, na_cores);
}

// Mask for the game's threads: every core it may use
int na_cores_thread_mask(void) {
    return na_cores == 4 ? (SCE_KERNEL_CPU_MASK_USER_0 | SCE_KERNEL_CPU_MASK_USER_1 | SCE_KERNEL_CPU_MASK_USER_2 |
                            SCE_KERNEL_CPU_MASK_USER_3)
                         : 0;
}

const char *na_cores_present_file(void) {
    return na_cores == 4 ? CPU_PRESENT4 : "app0:/cpu_present";
}
