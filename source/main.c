/*
 * Copyright (C) 2026 Vita port contributors
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

/**
 * @file  main.c
 * @brief Into the Dead (Unity 2020.3, IL2CPP) loader for the PS Vita.
 *
 * Data layout on the Vita (DATA_PATH = ux0:data/intothedead/):
 *   base.apk               the game's APK (Unity reads its assets from it)
 *   lib/libunity.so        lib/armeabi-v7a/libunity.so from the APK
 *   lib/libil2cpp.so       lib/armeabi-v7a/libil2cpp.so from the APK
 *   log.txt                written by the loader
 */

#include <string.h>

#include <psp2/ctrl.h>
#include <psp2/io/stat.h>
#include <psp2/io/fcntl.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/touch.h>

#include <vitaGL.h>
#include <kubridge.h>
#include <falso_jni/FalsoJNI.h>

#include "jni_objects.h"
#include "na_egl.h"
#include "na_input.h"
#include "na_looper.h"
#include "na_threads.h"
#include "na_hud.h"
#include "na_filecache.h"
#include "na_stats.h"
volatile na_frame_stats na_stats;

void na_java_tick(void); // na_java.c
void na_gl_log_memory(void); // na_gles3.c
#include "na_unity.h"
#include "utils/dialog.h"
#include "utils/glutil.h"
#include "utils/init.h"
#include "utils/logger.h"
#include "utils/utils.h"
#include "reimpl/controls.h"

// Unity's heap and il2cpp's GC heap come from newlib. The app gets ~365 MB in
// extended mode (ATTRIBUTE2=12); outside the heap the libraries need ~58 MB,
// plus a 41 MB temporary buffer while libil2cpp.so is read, plus stacks and
// vitaGL. 300 MB left too little for loading; with 192 MB the log showed
// 100 MB still free after loading, so 248 MB leaves ~44 MB of margin.
// 72 MB less than the Ninja Arashi port: Unity 2020.3's allocator regions get
// their memory outside the heap (reimpl/mem.c)
int _newlib_heap_size_user = 176 * 1024 * 1024;

#ifdef USE_SCELIBC_IO
int sceLibcHeapSize = 4 * 1024 * 1024;
#endif

// The main thread runs Unity's player loop: give it a big stack
unsigned int sceUserMainThreadStackSize = 4 * 1024 * 1024;

/*
 * Watchdog: when nothing is logged and no frame finishes for 30 s, write a
 * line and crash on purpose. The Vita then writes a core dump with every
 * thread's registers, which shows where the game hangs.
 */
extern volatile uint64_t log_last_activity_us;
static volatile uint64_t last_frame_us = 0;

static int watchdog_thread(SceSize args, void *argp) {
    while (1) {
        sceKernelDelayThread(1000 * 1000);
        log_flush();
        // read the last activity first: a frame finishing in between must not
        // look like one far in the future (that underflowed into a false alarm)
        uint64_t last = log_last_activity_us > last_frame_us ? log_last_activity_us : last_frame_us;
        uint64_t now = sceKernelGetProcessTimeWide();
        if (last > now) last = now;
        static int ticks = 0;
        if (++ticks % 10 == 0) { // every 10 s, also while stuck (doesn't count as progress)
            uint64_t saved = log_last_activity_us;
            na_gl_log_memory();
            log_last_activity_us = saved;
        }
        // L + R + SELECT held for 3 s: a core dump on request (shows where every thread is)
        static int combo_s = 0;
        SceCtrlData pad;
        sceCtrlPeekBufferPositive(0, &pad, 1);
        int combo = (pad.buttons & (SCE_CTRL_LTRIGGER | SCE_CTRL_RTRIGGER | SCE_CTRL_SELECT)) ==
                    (SCE_CTRL_LTRIGGER | SCE_CTRL_RTRIGGER | SCE_CTRL_SELECT);
        combo_s = combo ? combo_s + 1 : 0;
        if (combo_s >= 3 || (last && now - last > 45ULL * 1000 * 1000)) {
            if (combo_s >= 3) l_fatal("L+R+SELECT: crashing on purpose for a core dump.");
            else l_fatal("Watchdog: no progress for 45 s, crashing on purpose for a core dump.");
            na_gl_log_memory();
            na_threads_report();
            sceKernelDelayThread(200 * 1000);
            log_flush();
            kuKernelReleaseAbortHandler(); // a real crash, not one for a SIGSEGV handler
            *(volatile int *)0 = 0;
        }
    }
    return 0;
}

int main() {
    sceIoMkdir("ux0:data", 0777);
    sceIoMkdir(DATA_PATH, 0777);
    sceIoMkdir(DATA_PATH "files", 0777);
    sceIoMkdir(DATA_PATH "cache", 0777);
    sceIoMkdir(DATA_PATH "external", 0777);
    sceIoMkdir("ux0:data/shader_cache", 0777);
    // The shader cache (vitaGL's pack, failures included) is only valid for one
    // version of the shader translation: start over when it changes
    {
        static const char version[] = "translator-2";
        char old[32] = "";
        SceUID f = sceIoOpen(DATA_PATH "shader_cache_version.txt", SCE_O_RDONLY, 0);
        if (f >= 0) {
            sceIoRead(f, old, sizeof(old) - 1);
            sceIoClose(f);
        }
        if (strcmp(old, version) != 0) {
            sceIoRemove("ux0:data/shader_cache/ITDD00001.pack");
            f = sceIoOpen(DATA_PATH "shader_cache_version.txt", SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0777);
            if (f >= 0) {
                sceIoWrite(f, version, strlen(version));
                sceIoClose(f);
            }
        }
    }
    l_info("Into the Dead loader starting.");

    if (!file_exists(DATA_PATH "base.apk"))
        fatal_error("Missing game file:\n%sbase.apk\n\nCopy the game's APK there (renamed to base.apk).", DATA_PATH);

    soloader_init_all();
    jni_objects_install();
    l_info("Loader initialized.");

    sceTouchSetSamplingState(SCE_TOUCH_PORT_FRONT, SCE_TOUCH_SAMPLING_STATE_START);
    sceCtrlSetSamplingMode(SCE_CTRL_MODE_ANALOG_WIDE);

    gl_init();
    l_info("vitaGL initialized.");
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    vglSwapBuffers(GL_FALSE);

#ifndef NA_RELEASE
    SceUID wd = sceKernelCreateThread("watchdog", watchdog_thread, 0x10000100 + 10, 0x4000, 0, 0, NULL);
    if (wd >= 0) sceKernelStartThread(wd, 0, NULL);
#else
    (void)watchdog_thread;
#endif

    na_thread_register("main", (void *)main);
    na_unity_start();

    int frame = 0;
    while (1) {
        na_input_poll();
        na_looper_tick_main();
        na_java_tick();
        int swaps_before = na_egl_swap_count;
        int r = na_unity_frame();
#ifndef NA_RELEASE
        if (frame < 5 || frame % 600 == 0)
        {
            // speed of the last 600 frames, and what the port itself took of them
            static uint64_t t600 = 0;
            extern volatile uint32_t na_hud_us;
            uint64_t t = sceKernelGetProcessTimeWide();
            uint32_t hits = 0, kept = na_filecache_kb(&hits);
            l_info("Frame %d (nativeRender %d, swaps %d): %.1f fps, controls check %u us/frame, asset cache %u KB (%u KB read from it)",
                   frame, r, na_egl_swap_count, t600 && frame >= 600 ? 600.0e6 / (double)(t - t600) : 0.0,
                   (unsigned)(na_hud_us / 600), (unsigned)kept, (unsigned)hits);
            t600 = t;
            na_hud_us = 0;
            na_gl_log_memory();
        }
#endif
        na_filecache_tick(frame);
        // Unity skips the swap while it has nothing to show: keep the display alive
        if (na_egl_swap_count == swaps_before)
            sceKernelDelayThread(1000);
        if (frame == 0)
            sceIoRemove(DATA_PATH "sound_probe.txt"); // sound setup went fine (see dynlib.c)
        na_hud_tick(frame);
        frame++;
#ifndef NA_RELEASE
        uint64_t now_us = sceKernelGetProcessTimeWide();
        // Hitches: frames over 50 ms, with what they spent their time on
        static int hitches = 0;
        uint32_t ft = last_frame_us ? (uint32_t)((now_us - last_frame_us) / 1000) : 0;
        if (ft > 50 && frame > 300 && hitches < 300) {
            hitches++;
            l_info("[HITCH] frame %d: %u ms (textures %u in %u ms, shaders %u in %u ms, reads %u KB in %u ms, "
                   "swap %u ms, fence waits %u ms)", frame, ft, na_stats.tex_n, na_stats.tex_us / 1000,
                   na_stats.link_n, na_stats.link_us / 1000, na_stats.read_kb, na_stats.read_us / 1000,
                   na_stats.swap_us / 1000, na_stats.finish_us / 1000);
        }
        last_frame_us = now_us;
#endif
        memset((void *)&na_stats, 0, sizeof(na_stats));
    }

    sceKernelExitDeleteThread(0);
}

// Input is not wired up yet (see docs/NOTES.md); the boilerplate thread is unused.
void controls_handler_key(int32_t keycode, ControlsAction action) {}
void controls_handler_touch(int32_t id, float x, float y, ControlsAction action) {}
void controls_handler_analog(ControlsStickId which, float x, float y, ControlsAction action) {}
