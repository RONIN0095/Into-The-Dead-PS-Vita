/*
 * Copyright (C) 2026 Vita port contributors
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

/**
 * @file  na_pad.c
 * @brief The Vita's buttons and stick as the game's own controller.
 *
 * Into the Dead has controller support of its own: its
 * script_controllerInputManager walks menus button by button (highlight,
 * confirm, back, tabs) and its TouchControls2 has a CONTROLLER mode in which
 * steering and shooting are set from outside. On Android that is driven by
 * a DRGenericGamepad object reading Unity's joystick axes; which never
 * becomes "connected" by itself.
 *
 * Here script_controllerInputManager.Update() is replaced: the generic
 * gamepad is marked connected (the game then shows its controller UI and
 * switches the run to CONTROLLER mode), and each frame the Vita's pad is
 * read and the same calls are made that DRGenericGamepad makes:
 *
 *   menus:  D-pad / left stick  OnNext/OnPrevious, OnMoveUp/DownATier
 *           Cross  OnConfirm     Circle  OnDismiss     L/R  OnTabLeft/Right
 *   a run:  left stick or D-pad steer, tilting the console steers too
 *           (while the game's control option is "tilt"; SELECT turns
 *           tilting off and on), Cross / Square / R shoot, START pauses.
 *
 * Addresses are those of the game's libil2cpp.so 2.7.1 (armeabi-v7a); the
 * first instructions are checked before anything is hooked.
 */

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <psp2/ctrl.h>
#include <psp2/io/fcntl.h>
#include <psp2/kernel/clib.h>
#include <psp2/kernel/processmgr.h>

#include "na_modules.h"
#include "utils/logger.h"

#ifndef DATA_PATH
#define DATA_PATH "ux0:data/intothedead/"
#endif

// libil2cpp.so 2.7.1
#define RVA_MGR_UPDATE 0xA73740       // script_controllerInputManager.Update
#define RVA_MGR_CONNECTED 0xA73A48    // .OnControllerConnected
#define RVA_MGR_NEXT 0xA741F4
#define RVA_MGR_PREV 0xA74234
#define RVA_MGR_CONFIRM 0xA74274
#define RVA_MGR_DISMISS 0xA742B4
#define RVA_MGR_TAB_LEFT 0xA74870
#define RVA_MGR_TAB_RIGHT 0xA748B0
#define RVA_MGR_UP 0xA748F0
#define RVA_MGR_DOWN 0xA74950
#define RVA_MGR_PAUSE 0xA749B0
#define RVA_TC_SET_SHOOT 0x6982AC     // TouchControls2.SetShoot(bool)
#define RVA_TC_SWITCH 0x696D04        // TouchControls2.SwitchControls()
#define RVA_PROFILE_GET_LOCAL_INT 0x52C358 // PlayerProfile.GetLocalInt(LocalId)

// script_controllerInputManager
#define MGR_INPUT_MODE 0x0C // 0 menu, 1 popup, 2 gameplay
#define MGR_GENERIC 0x44    // DRGenericGamepad
#define GENERIC_CONNECTED 0x10
// TouchControls2
#define TC_TILT_CONST 0x1C
#define TC_TILT_VAR 0x20
#define TC_MODE 0x28 // 5: controller
#define TC_STEERING 0x30
#define TC_USED_STICK 0x48
// UIManager
#define UI_BLOCK_INPUT 0x3C
// UnityEngine.Object: the native object (null once destroyed)
#define OBJ_CACHED_PTR 0x08

#define U8(p, o) (*(uint8_t *)((char *)(p) + (o)))
#define I32(p, o) (*(int32_t *)((char *)(p) + (o)))
#define F32(p, o) (*(float *)((char *)(p) + (o)))
#define PTR(p, o) (*(void **)((char *)(p) + (o)))

typedef void (*method_t)(void *self, void *method);
typedef void (*method_b_t)(void *self, int arg, void *method);
typedef int (*static_i_t)(int arg, void *method);

static uintptr_t base;
#define FN(rva) ((void *)(base + (rva)))

int na_motion_gravity_x(float *x); // na_android.c

/*
 * Classes and their static fields, through the IL2CPP API
 */
static void *find_class(const char *name) {
    void *(*domain_get)(void) = (void *)na_module_symbol(NA_LIB_IL2CPP, "il2cpp_domain_get");
    void **(*get_assemblies)(void *, size_t *) = (void *)na_module_symbol(NA_LIB_IL2CPP, "il2cpp_domain_get_assemblies");
    void *(*get_image)(void *) = (void *)na_module_symbol(NA_LIB_IL2CPP, "il2cpp_assembly_get_image");
    void *(*from_name)(void *, const char *, const char *) = (void *)na_module_symbol(NA_LIB_IL2CPP, "il2cpp_class_from_name");
    if (!domain_get || !get_assemblies || !get_image || !from_name) return NULL;
    size_t n = 0;
    void **as = get_assemblies(domain_get(), &n);
    for (size_t i = 0; as && i < n; i++) {
        void *k = from_name(get_image(as[i]), "", name);
        if (k) return k;
    }
    return NULL;
}

// the object in a class's static field (a singleton's "instance"), or NULL
static void *static_object(void **klass, void **field, const char *class_name, const char *field_name) {
    static void *(*get_field)(void *, const char *);
    static void (*get_value)(void *, void *);
    if (!get_field) get_field = (void *)na_module_symbol(NA_LIB_IL2CPP, "il2cpp_class_get_field_from_name");
    if (!get_value) get_value = (void *)na_module_symbol(NA_LIB_IL2CPP, "il2cpp_field_static_get_value");
    if (!get_field || !get_value) return NULL;
    if (!*klass) {
        *klass = find_class(class_name);
        if (!*klass) return NULL;
        *field = get_field(*klass, field_name);
        l_info("[pad] %s: class %p, field %s %p", class_name, *klass, field_name, *field);
    }
    if (!*field) return NULL;
    void *obj = NULL;
    get_value(*field, &obj);
    if (obj && !PTR(obj, OBJ_CACHED_PTR)) return NULL; // destroyed
    return obj;
}

/*
 * Tilt on/off (SELECT), kept in a file
 */
#define TILT_FILE DATA_PATH "tilt_off.txt"
static int tilt_off = -1;
static int tilt_is_off(void) {
    if (tilt_off < 0) {
        SceUID fd = sceIoOpen(TILT_FILE, SCE_O_RDONLY, 0);
        tilt_off = fd >= 0;
        if (fd >= 0) sceIoClose(fd);
    }
    return tilt_off;
}
static void tilt_toggle(void) {
    tilt_off = !tilt_is_off();
    if (tilt_off) {
        SceUID fd = sceIoOpen(TILT_FILE, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0666);
        if (fd >= 0) sceIoClose(fd);
    } else {
        sceIoRemove(TILT_FILE);
    }
    l_info("[pad] tilt steering %s (SELECT)", tilt_off ? "off" : "on");
}

/*
 * The pad
 */
#define DIR_LEFT 1
#define DIR_RIGHT 2
#define DIR_UP 4
#define DIR_DOWN 8

static uint32_t prev_buttons;
static int prev_dirs, repeat_dir;
static uint64_t repeat_at;

// D-pad and stick as directions; one of them repeats while held
static int menu_dirs(const SceCtrlData *pad, uint64_t now, int *pressed) {
    int d = 0;
    if (pad->buttons & SCE_CTRL_LEFT) d |= DIR_LEFT;
    if (pad->buttons & SCE_CTRL_RIGHT) d |= DIR_RIGHT;
    if (pad->buttons & SCE_CTRL_UP) d |= DIR_UP;
    if (pad->buttons & SCE_CTRL_DOWN) d |= DIR_DOWN;
    int sx = (int)pad->lx - 128, sy = (int)pad->ly - 128;
    // the stick: past 70% it is a direction, the stronger axis only
    if (sx * sx + sy * sy > 90 * 90) {
        if (sx * sx >= sy * sy) d |= sx < 0 ? DIR_LEFT : DIR_RIGHT;
        else d |= sy < 0 ? DIR_UP : DIR_DOWN;
    }
    *pressed = d & ~prev_dirs;
    if (*pressed) {
        repeat_dir = *pressed;
        repeat_at = now + 450000;
    } else if (repeat_dir && (d & repeat_dir) == repeat_dir && now >= repeat_at) {
        *pressed = repeat_dir;
        repeat_at = now + 170000;
    } else if (!(d & repeat_dir)) {
        repeat_dir = 0;
    }
    prev_dirs = d;
    return d;
}

static float steer_from_pad(const SceCtrlData *pad, void *tc, int tilt_option) {
    if (pad->buttons & SCE_CTRL_LEFT) return -1.0f;
    if (pad->buttons & SCE_CTRL_RIGHT) return 1.0f;
    float x = ((int)pad->lx - 128) / 127.0f;
    const float dead = 0.18f;
    if (x > dead || x < -dead) {
        float v = (fabsf(x) - dead) / (1.0f - dead);
        if (v > 1.0f) v = 1.0f;
        return x < 0 ? -v : v;
    }
    if (!tilt_option || tilt_is_off()) return 0.0f;
    // Tilt, as the game's own tilt mode has it: the sideways part of gravity
    // over a range that grows with the screen size (the game's two constants)
    float gx;
    if (!na_motion_gravity_x(&gx)) return 0.0f;
    static float smooth = 0.0f;
    smooth += (gx - smooth) * 0.35f;
    float range = F32(tc, TC_TILT_CONST) + F32(tc, TC_TILT_VAR) * (960.0f / 220.0f);
    if (!(range > 0.05f && range < 1.5f)) range = 0.3f;
    static int logged = 0;
    if (!logged) {
        logged = 1;
        l_info("[pad] tilt range %.3f g (game: %.3f + %.3f per inch)", range, F32(tc, TC_TILT_CONST), F32(tc, TC_TILT_VAR));
    }
    float v = smooth / range;
    if (v > 1.0f) v = 1.0f;
    if (v < -1.0f) v = -1.0f;
    if (v > -0.04f && v < 0.04f) v = 0.0f;
    return v;
}

static void *tc_class, *tc_field, *ui_class, *ui_field;
static void *connected_mgr;
static int shooting;

/*
 * Replaces script_controllerInputManager.Update(): runs on Unity's main
 * thread, once a frame, like the game's own controller objects.
 */
static void pad_update(void *self, void *method) {
    (void)method;
    void *generic = PTR(self, MGR_GENERIC);
    if (!generic) return; // Start() has not run yet
    if (!U8(generic, GENERIC_CONNECTED) || connected_mgr != self) {
        U8(generic, GENERIC_CONNECTED) = 1;
        connected_mgr = self;
        l_info("[pad] controller connected to the game's input manager %p (input mode %d)", self, I32(self, MGR_INPUT_MODE));
        ((method_t)FN(RVA_MGR_CONNECTED))(self, NULL);
    }

    SceCtrlData pad;
    memset(&pad, 0, sizeof(pad));
    if (sceCtrlPeekBufferPositive(0, &pad, 1) < 0) return;
    uint64_t now = sceKernelGetProcessTimeWide();
    uint32_t down = pad.buttons & ~prev_buttons;
    prev_buttons = pad.buttons;
    int dir_pressed = 0;
    menu_dirs(&pad, now, &dir_pressed);

    int mode = I32(self, MGR_INPUT_MODE);
    static int last_mode = -1, modes_logged = 0;
    if (mode != last_mode && modes_logged < 40) {
        modes_logged++;
        l_info("[pad] input mode %d (0 menu, 1 popup, 2 run)", mode);
    }
    last_mode = mode;

    // SELECT, tapped alone: tilt off/on (START + SELECT held leaves the game, main.c)
    static uint64_t select_since = 0;
    static int select_with_start = 0;
    if (pad.buttons & SCE_CTRL_SELECT) {
        if (!select_since) {
            select_since = now;
            select_with_start = 0;
        }
        if (pad.buttons & SCE_CTRL_START) select_with_start = 1;
    } else if (select_since) {
        if (!select_with_start && now - select_since < 700000) tilt_toggle();
        select_since = 0;
    }
    if ((down & SCE_CTRL_START) && !(pad.buttons & SCE_CTRL_SELECT)) ((method_t)FN(RVA_MGR_PAUSE))(self, NULL);

    void *tc = static_object(&tc_class, &tc_field, "TouchControls2", "instance");
    int shoot_now = 0;
    // One frame of a run with every draw in the log (as L+R+DOWN does), to see
    // what the GPU's time goes into: 600 frames into the first run
    {
        extern volatile int na_trace_request;
        static int run_frames = 0, traced = 0;
        if (0 && mode == 2 && !traced && ++run_frames == 600) { // off: it stopped the run for 10 s
            traced = 1;
            na_trace_request = 1;
            l_info("[pad] tracing one frame of the run");
        }
    }
    if (mode == 2 && tc) {
        static void *switched_tc;
        if (I32(tc, TC_MODE) != 5 && switched_tc != tc) { // once for each run's controls
            switched_tc = tc;
            l_info("[pad] the run's controls are in mode %d: switching to the controller mode", I32(tc, TC_MODE));
            ((method_t)FN(RVA_TC_SWITCH))(tc, NULL);
        }
        if (I32(tc, TC_MODE) == 5) {
            // the game's control option: 0 is tilt (asked for now and then)
            static int tilt_option = 1, asked = 0;
            if (asked++ % 60 == 0) tilt_option = ((static_i_t)FN(RVA_PROFILE_GET_LOCAL_INT))(0, NULL) == 0;
            U8(tc, TC_USED_STICK) = 1; // the steering value is ours: no easing back to 0 by the game
            F32(tc, TC_STEERING) = steer_from_pad(&pad, tc, tilt_option);
            shoot_now = (pad.buttons & (SCE_CTRL_CROSS | SCE_CTRL_SQUARE | SCE_CTRL_RTRIGGER)) != 0;
        }
    }
    if (tc && shoot_now != shooting) ((method_b_t)FN(RVA_TC_SET_SHOOT))(tc, shoot_now, NULL);
    if (tc || !shoot_now) shooting = shoot_now;
    if (mode == 2) return;

    // Menus and pop-ups (not while the UI itself blocks input, as the game has it)
    void *ui = static_object(&ui_class, &ui_field, "UIManager", "s_Instance");
    if (ui && U8(ui, UI_BLOCK_INPUT)) return;
    if (dir_pressed & DIR_RIGHT) ((method_t)FN(RVA_MGR_NEXT))(self, NULL);
    if (dir_pressed & DIR_LEFT) ((method_t)FN(RVA_MGR_PREV))(self, NULL);
    if (dir_pressed & DIR_UP) ((method_t)FN(RVA_MGR_UP))(self, NULL);
    if (dir_pressed & DIR_DOWN) ((method_t)FN(RVA_MGR_DOWN))(self, NULL);
    if (down & SCE_CTRL_CROSS) ((method_t)FN(RVA_MGR_CONFIRM))(self, NULL);
    if (down & SCE_CTRL_CIRCLE) ((method_t)FN(RVA_MGR_DISMISS))(self, NULL);
    if (down & SCE_CTRL_LTRIGGER) ((method_t)FN(RVA_MGR_TAB_LEFT))(self, NULL);
    if (down & SCE_CTRL_RTRIGGER) ((method_t)FN(RVA_MGR_TAB_RIGHT))(self, NULL);
}

int na_pad_active = 0;

/** After libil2cpp.so is relocated, before its caches are flushed. */
void na_pad_install(void) {
    NaLib *lib = &na_libs[NA_LIB_IL2CPP];
    base = lib->load_addr;
    // push {r4, r10, r11, lr} / strb r1, [r0, #0x2d]: the functions expected there
    uint32_t update0 = *(uint32_t *)FN(RVA_MGR_UPDATE), shoot0 = *(uint32_t *)FN(RVA_TC_SET_SHOOT);
    if (update0 != 0xe92d4c10 || shoot0 != 0xe5c0102d) {
        l_warn("[pad] another libil2cpp.so than 2.7.1 (0x%08x, 0x%08x): buttons stay off, touch works", update0, shoot0);
        return;
    }
    hook_addr((uintptr_t)FN(RVA_MGR_UPDATE), (uintptr_t)&pad_update);
    na_pad_active = 1;
    l_info("[pad] the game's controller manager reads the Vita's buttons");
}
