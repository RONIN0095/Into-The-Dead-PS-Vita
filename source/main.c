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
#include "na_trace.h"
#include "na_modules.h"
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
// 56 MB: the intro scene left 43 MB of the 80 MB heap untouched while the
// reservations' pool ran out; the pool got the difference (reimpl/mem.c)
int _newlib_heap_size_user = 52 * 1024 * 1024; // (56 MB before: 14 MB of it were never touched when everything else ran out)
// // Unity used 36 MB of it; the rest of its memory is reservations

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
static SceUID main_tid = -1;

/*
 * The way out: the memory reservations are given back before the process
 * ends (reimpl/mem.c, na_mmap_teardown). Closing the game from the LiveArea
 * ends it without this.
 */
// Time spent in each main loop phase since the last hitch line (which says
// where a slow frame outside Unity's own frame went)
static uint32_t phase_us[9];
static uint64_t phase_t0;
#define NA_PHASE(n) do { uint64_t _t = sceKernelGetProcessTimeWide(); \
    if (phase_t0 && na_main_phase >= 0 && na_main_phase < 9) phase_us[na_main_phase] += (uint32_t)(_t - phase_t0); \
    phase_t0 = _t; na_main_phase = (n); } while (0)

static void clean_exit(const char *why) {
    void na_mmap_teardown(void);
    l_info("Leaving (%s).", why);
    log_flush();
    log_sync();
    na_mmap_teardown(); // logs each step (synced), to see where it stops if it does
    l_info("Leaving: process ends.");
    log_flush();
    log_sync();
    sceKernelExitProcess(0);
}

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
            { void na_apkcache_report(void); na_apkcache_report(); }
            log_last_activity_us = saved;
        }
        // L + R + SELECT held for 3 s: a core dump on request (shows where every thread is)
        static int combo_s = 0;
        SceCtrlData pad;
        sceCtrlPeekBufferPositive(0, &pad, 1);
        int combo = (pad.buttons & (SCE_CTRL_LTRIGGER | SCE_CTRL_RTRIGGER | SCE_CTRL_SELECT)) ==
                    (SCE_CTRL_LTRIGGER | SCE_CTRL_RTRIGGER | SCE_CTRL_SELECT);
        combo_s = combo ? combo_s + 1 : 0;
        // After 10 s without progress: the main thread's last calls into the
        // loader (na_trace.c), written through to the memory card at once, so
        // the log keeps them even if the console goes down afterwards. Nothing
        // else: no stack scans, no locks.
        static uint64_t reported_for = 0;
        if (last && now - last > 10ULL * 1000 * 1000 && reported_for != last) {
            reported_for = last;
            uint64_t saved = log_last_activity_us;
            l_warn("Watchdog: no progress for 10 s");
            na_trace_report();
            log_flush();
            log_sync();
            na_threads_report(); // what each thread waits for (GC stops: "sigsuspend")
            log_flush();
            log_sync();
            log_last_activity_us = saved;
        }
        if (combo_s >= 3 || (last && now - last > 45ULL * 1000 * 1000)) {
            // No crash on purpose any more: the Vita's core dump of this
            // port hung at 0% and the console rebooted. The thread report
            // and the trace above say where the game hangs; it exits.
            if (combo_s >= 3) l_fatal("L+R+SELECT: exit.");
            else l_fatal("Watchdog: no progress for 45 s, exit.");
            log_flush();
            log_sync();
            sceKernelExitProcess(0);
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
    l_info("Into the Dead loader starting (build %s).", NA_BUILD_ID);

    if (!file_exists(DATA_PATH "base.apk"))
        fatal_error("Missing game file:\n%sbase.apk\n\nCopy the game's APK there (renamed to base.apk).", DATA_PATH);

    { void na_cores_probe(void); na_cores_probe(); } // before Unity counts the cores
    soloader_init_all();
    jni_objects_install();
    l_info("Loader initialized.");

    sceTouchSetSamplingState(SCE_TOUCH_PORT_FRONT, SCE_TOUCH_SAMPLING_STATE_START);
    sceCtrlSetSamplingMode(SCE_CTRL_MODE_ANALOG_WIDE);

    void na_mmap_pool_init(void); // reimpl/mem.c
    na_mmap_pool_init();
    gl_init();
    l_info("vitaGL initialized.");
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    vglSwapBuffers(GL_FALSE);

#ifndef NA_RELEASE
    SceUID wd = sceKernelCreateThread("watchdog", watchdog_thread, 0x10000100 - 20, 0x4000, 0, 0, NULL);
    if (wd >= 0) sceKernelStartThread(wd, 0, NULL);
#else
    (void)watchdog_thread;
#endif

    na_thread_register("main", (void *)main);
    main_tid = sceKernelGetThreadId();
#ifndef NA_RELEASE
    na_trace_start(); // records the main thread's calls into the loader (hang reports)
#endif
    NA_PHASE(1);
    na_unity_start();

    int frame = 0;
    while (1) {
        NA_PHASE(2);
        {
            // START + SELECT held for a second: leave (in order, see clean_exit)
            static uint64_t held_since = 0;
            SceCtrlData pad;
            memset(&pad, 0, sizeof(pad));
            sceCtrlPeekBufferPositive(0, &pad, 1);
            int both = (pad.buttons & (SCE_CTRL_START | SCE_CTRL_SELECT)) == (SCE_CTRL_START | SCE_CTRL_SELECT);
            if (!both) held_since = 0;
            else if (!held_since) held_since = sceKernelGetProcessTimeWide();
            else if (sceKernelGetProcessTimeWide() - held_since > 1000000ULL) clean_exit("START + SELECT");
            static int combo_logged = 0;
            if (both && !combo_logged) {
                combo_logged = 1;
                l_info("START + SELECT pressed: leaving after 1 s");
                log_flush();
                log_sync();
            }
        }
        { void na_sig_poll(void); na_sig_poll(); } // signals for the main thread (GC stops)
        na_input_poll();
        NA_PHASE(3);
        na_looper_tick_main();
        NA_PHASE(4);
        na_java_tick();
        int swaps_before = na_egl_swap_count;
        NA_PHASE(5);
        uint64_t unity_t0 = sceKernelGetProcessTimeWide();
        int r = na_unity_frame();
        uint32_t unity_us = (uint32_t)(sceKernelGetProcessTimeWide() - unity_t0);
        (void)unity_us;
        { void na_sensor_service(void); na_sensor_service(); } // accelerometer events (tilt)
#ifndef NA_RELEASE
        { void na_input_probe(void); na_input_probe(); } // what Unity makes of touches
#endif
        // nativeRender false for good: Unity has quit (Application.Quit,
        // Back in the main menu). Leave instead of looping on.
        static uint64_t quit_since = 0;
        if (r) quit_since = 0;
        else if (!quit_since) quit_since = sceKernelGetProcessTimeWide();
        else if (sceKernelGetProcessTimeWide() - quit_since > 3000000ULL) {
            clean_exit("Unity has quit: nativeRender false for 3 s");
        }
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
        if (frame % 30 == 0) {
            // managed heap size for the memory log (IL2CPP API, main thread only)
            extern volatile unsigned na_gc_heap_kb, na_gc_used_kb;
            static int64_t (*heap_size)(void), (*used_size)(void);
            static int looked = 0;
            if (!looked) {
                looked = 1;
                heap_size = (void *)na_module_symbol(NA_LIB_IL2CPP, "il2cpp_gc_get_heap_size");
                used_size = (void *)na_module_symbol(NA_LIB_IL2CPP, "il2cpp_gc_get_used_size");
            }
            if (heap_size) na_gc_heap_kb = (unsigned)(heap_size() / 1024);
            if (used_size) na_gc_used_kb = (unsigned)(used_size() / 1024);
        }
        NA_PHASE(6);
        na_filecache_tick(frame);
        // Unity skips the swap while it has nothing to show: keep the display alive
        if (na_egl_swap_count == swaps_before)
            sceKernelDelayThread(1000);
        if (frame == 0)
            sceIoRemove(DATA_PATH "sound_probe.txt"); // sound setup went fine (see dynlib.c)
        NA_PHASE(7);
        na_hud_tick(frame);
        frame++;
#ifndef NA_RELEASE
        uint64_t now_us = sceKernelGetProcessTimeWide();
        // Hitches: frames over 50 ms, with what they spent their time on
        static int hitches = 0;
        uint32_t ft = last_frame_us ? (uint32_t)((now_us - last_frame_us) / 1000) : 0;
        if ((ft > 1000 && frame > 4) || (ft > 40 && frame > 300 && hitches < 400)) { // loads always, slow frames up to 400
            hitches++;
            { void na_apkcache_report(void); na_apkcache_report(); }
            {
                extern volatile uint32_t na_tc_open_us, na_tc_read_us, na_tc_unzip_us, na_tc_n;
                if (na_tc_n)
                    l_info("[HITCH] texture cache: %u files, open %u ms, read %u ms, unpack %u ms", na_tc_n,
                           na_tc_open_us / 1000, na_tc_read_us / 1000, na_tc_unzip_us / 1000);
                na_tc_n = na_tc_open_us = na_tc_read_us = na_tc_unzip_us = 0;
            }
            l_info("[HITCH] phases (ms): input %u, looper %u, java %u, unity %u, cache %u, hud %u, rest %u",
                   phase_us[2] / 1000, phase_us[3] / 1000, phase_us[4] / 1000, phase_us[5] / 1000, phase_us[6] / 1000,
                   phase_us[7] / 1000, (phase_us[0] + phase_us[1] + phase_us[8]) / 1000);
            l_info("[HITCH] frame %d: %u ms (Unity frame %u ms, draws %u in %u ms [checks %u us, %u vertex programs %u us, "
                   "%u fragment programs %u us], textures %u in %u ms, shaders %u in %u ms, "
                   "reads %u KB in %u ms, swap %u ms, fence waits %u ms)", frame, ft, unity_us / 1000, na_stats.draw_n,
                   na_stats.draw_us / 1000, na_stats.check_us, na_stats.vp_n, na_stats.vp_us, na_stats.fp_n, na_stats.fp_us,
                   na_stats.tex_n, na_stats.tex_us / 1000,
                   na_stats.link_n, na_stats.link_us / 1000, na_stats.read_kb, na_stats.read_us / 1000,
                   na_stats.swap_us / 1000, na_stats.finish_us / 1000);
        }
        last_frame_us = now_us;
#endif
        memset((void *)&na_stats, 0, sizeof(na_stats));
        memset(phase_us, 0, sizeof(phase_us));
    }

    sceKernelExitDeleteThread(0);
}

// Input is not wired up yet (see docs/NOTES.md); the boilerplate thread is unused.
void controls_handler_key(int32_t keycode, ControlsAction action) {}
void controls_handler_touch(int32_t id, float x, float y, ControlsAction action) {}
void controls_handler_analog(ControlsStickId which, float x, float y, ControlsAction action) {}
