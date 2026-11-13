/*
 * Copyright (C) 2026 Vita port contributors
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

/**
 * @file  na_input.c
 * @brief Front touch and buttons as Android input events for Unity.
 *
 * Unity gets input through UnityPlayer.nativeInjectEvent(InputEvent, int).
 * The events here are our Java objects (jni_objects.c) with fields; Unity
 * reads them through MotionEvent/KeyEvent getters, which na_java.c answers
 * via na_input_event_call().
 *
 * Touch: one MotionEvent per change, like Android (DOWN, POINTER_DOWN, MOVE,
 * POINTER_UP, UP). Buttons: KeyEvents with gamepad key codes.
 */

#include "na_input.h"

#include <stdio.h>
#include <string.h>

#include <psp2/ctrl.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/touch.h>

#include "jni_objects.h"
#include "na_android.h"
#include "na_unity.h"
#include "na_hud.h"
#include "utils/logger.h"

#define ACTION_DOWN 0
#define ACTION_UP 1
#define ACTION_MOVE 2
#define ACTION_POINTER_DOWN 5
#define ACTION_POINTER_UP 6

#define SOURCE_TOUCHSCREEN 0x00001002
#define SOURCE_GAMEPAD 0x00000401
#define SOURCE_KEYBOARD 0x00000101

#define MAX_POINTERS 10

typedef struct {
    int id;
    float x, y;
} touch_point;

static touch_point prev[MAX_POINTERS];
static int n_prev = 0;
static jlong down_time = 0;

// Unity may keep events a little while after nativeInjectEvent returns:
// free them a while later
#define KEEP_EVENTS 128
static jobject old_events[KEEP_EVENTS];
static int old_pos = 0;

static void retire(jobject ev) {
    if (old_events[old_pos]) jo_free_object(old_events[old_pos]);
    old_events[old_pos] = ev;
    old_pos = (old_pos + 1) % KEEP_EVENTS;
}

static jlong now_ms(void) {
    return (jlong)(sceKernelGetProcessTimeWide() / 1000);
}

static void send_motion(int action, int action_index, const touch_point *pts, int n) {
    jobject ev = jo_new_object("android/view/MotionEvent");
    jo_set_int(ev, "__action", action | (action_index << 8));
    jo_set_int(ev, "__count", n);
    for (int i = 0; i < n && i < MAX_POINTERS; i++) {
        char k[16];
        snprintf(k, sizeof(k), "__id%d", i);
        jo_set_int(ev, k, pts[i].id);
        snprintf(k, sizeof(k), "__x%d", i);
        jo_set_float(ev, k, pts[i].x);
        snprintf(k, sizeof(k), "__y%d", i);
        jo_set_float(ev, k, pts[i].y);
    }
    jo_set_int(ev, "__source", SOURCE_TOUCHSCREEN);
    jo_set_long(ev, "__time", now_ms());
    jo_set_long(ev, "__downtime", down_time);
    na_unity_inject_event(ev);
    retire(ev);
}


/*
 * Buttons -> touches on the game's on-screen controls. The game reads only
 * its touch buttons (no key or gamepad input), so a held button is a finger
 * held on the matching control. Positions come from the level scenes'
 * canvas (CanvasScaler: 800x600 reference, matched to width -> x1.2 on the
 * 960x544 screen; Unity's y is measured from the bottom):
 *   Left  (0,0)+(67.3,77)     Right (0,0)+(201.4,77.2)
 *   Up/jump (1,0)+(-61.1,60)  Fi/shuriken (1,0)+(-51.5,214.5)
 *   Dash (1,0)+(-224.9,48)    Trans (1,0)+(-166.2,164.1)
 *   Check (0,0.5)+(49.9,0)    Back/pause (1,1)+(-29.1,-29.4)
 */
#define VPAD_LEFT 0x10000000u // left stick pushed left/right
#define VPAD_RIGHT 0x20000000u
static const struct {
    uint32_t buttons; // any of these
    float x, y;       // screen pixels, y from the top
} vpad[] = {
    { SCE_CTRL_LEFT | VPAD_LEFT, 80.8f, 451.6f },     // Left
    { SCE_CTRL_RIGHT | VPAD_RIGHT, 241.7f, 451.4f },  // Right
    { SCE_CTRL_CROSS | SCE_CTRL_UP, 886.7f, 472.0f }, // jump
    { SCE_CTRL_SQUARE, 898.2f, 286.6f },              // shuriken
    { SCE_CTRL_CIRCLE | SCE_CTRL_RTRIGGER, 690.1f, 486.4f }, // dash
    { SCE_CTRL_TRIANGLE, 760.6f, 347.1f },            // transform
    { SCE_CTRL_LTRIGGER, 59.9f, 272.0f },             // check
    { SCE_CTRL_START, 925.1f, 35.3f },                // pause
};
#define N_VPAD (int)(sizeof(vpad) / sizeof(vpad[0]))

volatile int na_trace_request = 0;

static SceCtrlData last_pad;

static uint32_t vpad_buttons(void) {
    SceCtrlData pad;
    memset(&pad, 0, sizeof(pad));
    if (sceCtrlPeekBufferPositive(0, &pad, 1) < 0) return 0;
    last_pad = pad;
    uint32_t b = pad.buttons;
    // L+R+DOWN: log every draw of the next frame and take a screenshot
    extern volatile int na_trace_request;
    static int trace_combo = 0;
    int combo = (b & (SCE_CTRL_LTRIGGER | SCE_CTRL_RTRIGGER | SCE_CTRL_DOWN)) ==
                (SCE_CTRL_LTRIGGER | SCE_CTRL_RTRIGGER | SCE_CTRL_DOWN);
#ifndef NA_RELEASE
    if (combo && !trace_combo) na_trace_request = 1;
#endif
    trace_combo = combo;
    if (pad.lx < 64) b |= VPAD_LEFT;
    if (pad.lx > 192) b |= VPAD_RIGHT;
    // L+R+SELECT is the watchdog's core dump combo: no touches for it
    if ((b & (SCE_CTRL_LTRIGGER | SCE_CTRL_RTRIGGER | SCE_CTRL_SELECT)) ==
        (SCE_CTRL_LTRIGGER | SCE_CTRL_RTRIGGER | SCE_CTRL_SELECT))
        return 0;
    return b;
}

/*
 * Menus: a cursor moved with the D-pad or the left stick; Cross taps where
 * it is (a finger held while Cross is held, so sliders and scroll lists
 * work too). It shows while it is used and hides after 6 s, or when the
 * screen is touched.
 */
static float cursor_x = SCREEN_W / 2, cursor_y = SCREEN_H / 2;
static uint64_t cursor_until = 0;

static void menu_cursor(uint32_t b, int touched) {
    float dx = 0, dy = 0;
    if (b & SCE_CTRL_LEFT) dx -= 7;
    if (b & SCE_CTRL_RIGHT) dx += 7;
    if (b & SCE_CTRL_UP) dy -= 7;
    if (b & SCE_CTRL_DOWN) dy += 7;
    int sx = (int)last_pad.lx - 128, sy = (int)last_pad.ly - 128;
    if (sx > 24 || sx < -24) dx += sx / 10.0f;
    if (sy > 24 || sy < -24) dy += sy / 10.0f;
    uint64_t now = sceKernelGetProcessTimeWide();
    if (dx || dy || (b & SCE_CTRL_CROSS)) cursor_until = now + 6000000;
    if (touched) cursor_until = 0;
    cursor_x += dx;
    cursor_y += dy;
    if (cursor_x < 0) cursor_x = 0;
    if (cursor_x > SCREEN_W - 1) cursor_x = SCREEN_W - 1;
    if (cursor_y < 0) cursor_y = 0;
    if (cursor_y > SCREEN_H - 1) cursor_y = SCREEN_H - 1;
}

// Called before each swap: draws the cursor while it is in use
void na_input_draw_cursor(void) {
    void na_gl_draw_cursor(int x, int y);
    if (cursor_until && sceKernelGetProcessTimeWide() < cursor_until && !na_hud_in_level())
        na_gl_draw_cursor((int)cursor_x, (int)cursor_y);
}

/*
 * The pop-up's X is tapped 3 times at most (8 frames down, 22 up), and not
 * at all once the screen is touched or a button pressed: then the pop-up
 * is the player's, with the cursor (Cross) or a finger, Accept included.
 */
static int gdpr_t = 0, gdpr_manual = 0;
static int gdpr_auto(uint32_t b, int touched) {
    if (touched || b) gdpr_manual = 1;
    if (gdpr_manual || gdpr_t >= 3 * 30) return 0;
    return 1;
}

static void poll_touch(void) {
    SceTouchData td;
    memset(&td, 0, sizeof(td));
    if (sceTouchPeek(SCE_TOUCH_PORT_FRONT, &td, 1) < 0) return;

    touch_point now[MAX_POINTERS];
    int n_now = 0;
    for (int i = 0; i < (int)td.reportNum && n_now < MAX_POINTERS; i++) {
        now[n_now].id = td.report[i].id & 0x7f;
        now[n_now].x = td.report[i].x * (float)SCREEN_W / 1920.0f;
        now[n_now].y = td.report[i].y * (float)SCREEN_H / 1088.0f;
        n_now++;
    }
    uint32_t b = vpad_buttons();
    if (na_hud_in_level()) {
        cursor_until = 0;
        for (int k = 0; k < N_VPAD && n_now < MAX_POINTERS; k++) {
            if (!(b & vpad[k].buttons)) continue;
            now[n_now].id = 128 + k; // real fingers are 0..127
            now[n_now].x = vpad[k].x;
            now[n_now].y = vpad[k].y;
            n_now++;
        }
    } else if (na_hud_gdpr_up() && gdpr_auto(b, td.reportNum > 0)) {
        /*
         * The privacy pop-up at the start: its X ("do not accept", the
         * privacy-friendly answer; there is no network anyway) is tapped,
         * 8 frames down, 22 up, until the pop-up is gone. Canvas/GDPR/
         * GDPR Popup: 234.6x261.5 at the centre, X at (1,1)+(-20.8,-16.3),
         * 800x600 reference matched to width (x1.2).
         */
        if (gdpr_t++ % 30 < 8 && n_now < MAX_POINTERS) {
            now[n_now].id = 201;
            now[n_now].x = SCREEN_W / 2 + (234.6f / 2 - 20.8f) * 1.2f;
            now[n_now].y = SCREEN_H / 2 - (261.5f / 2 - 16.3f) * 1.2f;
            n_now++;
        }
    } else {
        menu_cursor(b, td.reportNum > 0);
        if ((b & SCE_CTRL_CROSS) && n_now < MAX_POINTERS) {
            now[n_now].id = 200; // the cursor's finger
            now[n_now].x = cursor_x;
            now[n_now].y = cursor_y;
            n_now++;
        }
    }

    // Lifted fingers: POINTER_UP (or UP for the last one), sent while the
    // pointer is still in the list, like Android
    touch_point cur[MAX_POINTERS];
    int n_cur = n_prev;
    memcpy(cur, prev, sizeof(prev));
    for (int p = 0; p < n_cur;) {
        int still = 0;
        for (int i = 0; i < n_now; i++)
            if (now[i].id == cur[p].id) still = 1;
        if (still) {
            p++;
            continue;
        }
        send_motion(n_cur == 1 ? ACTION_UP : ACTION_POINTER_UP, p, cur, n_cur);
        memmove(&cur[p], &cur[p + 1], sizeof(touch_point) * (n_cur - p - 1));
        n_cur--;
    }

    // New fingers: DOWN (first) or POINTER_DOWN
    for (int i = 0; i < n_now; i++) {
        int known = 0;
        for (int p = 0; p < n_cur; p++)
            if (cur[p].id == now[i].id) known = 1;
        if (known || n_cur >= MAX_POINTERS) continue;
        cur[n_cur++] = now[i];
        if (n_cur == 1) down_time = now_ms();
        send_motion(n_cur == 1 ? ACTION_DOWN : ACTION_POINTER_DOWN, n_cur - 1, cur, n_cur);
    }

    // Movement
    int moved = 0;
    for (int p = 0; p < n_cur; p++)
        for (int i = 0; i < n_now; i++)
            if (cur[p].id == now[i].id && (cur[p].x != now[i].x || cur[p].y != now[i].y)) {
                cur[p] = now[i];
                moved = 1;
            }
    if (moved) send_motion(ACTION_MOVE, 0, cur, n_cur);

    memcpy(prev, cur, sizeof(cur));
    n_prev = n_cur;
}

/*
 * Buttons -> KeyEvents (Android key codes)
 */
static const struct {
    uint32_t button;
    int keycode;
} keymap[] = {
    { SCE_CTRL_CROSS, 96 },     // BUTTON_A
    { SCE_CTRL_CIRCLE, 97 },    // BUTTON_B
    { SCE_CTRL_SQUARE, 99 },    // BUTTON_X
    { SCE_CTRL_TRIANGLE, 100 }, // BUTTON_Y
    { SCE_CTRL_LTRIGGER, 102 }, // BUTTON_L1
    { SCE_CTRL_RTRIGGER, 103 }, // BUTTON_R1
    { SCE_CTRL_START, 108 },    // BUTTON_START
    { SCE_CTRL_SELECT, 4 },     // BACK
    { SCE_CTRL_UP, 19 },        // DPAD_UP
    { SCE_CTRL_DOWN, 20 },      // DPAD_DOWN
    { SCE_CTRL_LEFT, 21 },      // DPAD_LEFT
    { SCE_CTRL_RIGHT, 22 },     // DPAD_RIGHT
};

static uint32_t prev_buttons = 0;

static void send_key(int action, int keycode) {
    jobject ev = jo_new_object("android/view/KeyEvent");
    jo_set_int(ev, "__action", action);
    jo_set_int(ev, "__keycode", keycode);
    jo_set_int(ev, "__source", SOURCE_GAMEPAD | SOURCE_KEYBOARD);
    jo_set_long(ev, "__time", now_ms());
    jo_set_long(ev, "__downtime", now_ms());
    na_unity_inject_event(ev);
    retire(ev);
}

static void poll_buttons(void) {
    SceCtrlData pad;
    memset(&pad, 0, sizeof(pad));
    if (sceCtrlPeekBufferPositive(0, &pad, 1) < 0) return;
    uint32_t changed = pad.buttons ^ prev_buttons;
    for (unsigned i = 0; i < sizeof(keymap) / sizeof(keymap[0]); i++)
        if (changed & keymap[i].button)
            send_key((pad.buttons & keymap[i].button) ? 0 : 1, keymap[i].keycode);
    // menus: Circle is Android's Back (Unity's Escape) as well
    if ((changed & SCE_CTRL_CIRCLE) && !na_hud_in_level())
        send_key((pad.buttons & SCE_CTRL_CIRCLE) ? 0 : 1, 4);
    prev_buttons = pad.buttons;
}

void na_input_poll(void) {
    poll_touch();
    poll_buttons();
}

/*
 * MotionEvent.obtain(event) / KeyEvent(event): Unity copies every event it
 * gets before queueing it; the copy is retired like our own events.
 */
int na_input_static_call(const char *cls, const char *name, const jvalue *args, int argc, jvalue *r) {
    int motion = strcmp(cls, "android/view/MotionEvent") == 0;
    int key = strcmp(cls, "android/view/KeyEvent") == 0;
    if ((!motion && !key) || argc != 1 || !jo_is_object(args[0].l)) return 0;
    if (strcmp(name, "obtain") != 0 && strcmp(name, "obtainNoHistory") != 0 && strcmp(name, "changeAction") != 0)
        return 0;
    r->l = jo_clone_object(args[0].l);
    retire(r->l);
    return 1;
}

/*
 * Getters Unity calls on our events (from jo_java_call)
 */
int na_input_event_call(const char *name, const char *sig, jobject self, const jvalue *args, int argc, jvalue *r) {
    const char *cls = jo_class_name(self);
    int motion = strcmp(cls, "android/view/MotionEvent") == 0;
    int key = strcmp(cls, "android/view/KeyEvent") == 0;
    if (!motion && !key) return 0;
    int idx = (argc >= 1 && sig[1] == 'I') ? args[0].i : 0;
    char k[16];

    if (!strcmp(name, "getAction")) r->i = jo_get_int(self, "__action");
    else if (!strcmp(name, "getActionMasked")) r->i = jo_get_int(self, "__action") & 0xff;
    else if (!strcmp(name, "getActionIndex")) r->i = (jo_get_int(self, "__action") >> 8) & 0xff;
    else if (!strcmp(name, "getPointerCount")) r->i = motion ? jo_get_int(self, "__count") : 0;
    else if (!strcmp(name, "getPointerId")) {
        snprintf(k, sizeof(k), "__id%d", idx);
        r->i = jo_get_int(self, k);
    } else if (!strcmp(name, "findPointerIndex")) {
        r->i = -1;
        for (int i = 0; i < jo_get_int(self, "__count"); i++) {
            snprintf(k, sizeof(k), "__id%d", i);
            if (jo_get_int(self, k) == args[0].i) r->i = i;
        }
    } else if (!strcmp(name, "getX") || !strcmp(name, "getRawX")) {
        snprintf(k, sizeof(k), "__x%d", idx);
        r->f = jo_get_float(self, k);
    } else if (!strcmp(name, "getY") || !strcmp(name, "getRawY")) {
        snprintf(k, sizeof(k), "__y%d", idx);
        r->f = jo_get_float(self, k);
    } else if (!strcmp(name, "getAxisValue")) {
        int axis = args[0].i;
        int i = argc >= 2 ? args[1].i : 0;
        snprintf(k, sizeof(k), axis == 1 ? "__y%d" : "__x%d", i);
        r->f = (axis == 0 || axis == 1) ? jo_get_float(self, k) : 0.0f;
    } else if (!strcmp(name, "getPressure") || !strcmp(name, "getSize")) r->f = 1.0f;
    else if (!strcmp(name, "getTouchMajor") || !strcmp(name, "getTouchMinor")) r->f = 10.0f;
    else if (!strcmp(name, "getToolType")) r->i = 1; // TOOL_TYPE_FINGER
    else if (!strcmp(name, "getSource")) r->i = jo_get_int(self, "__source");
    else if (!strcmp(name, "getDeviceId")) r->i = motion ? 1 : 2;
    else if (!strcmp(name, "getEventTime")) r->j = jo_get_long(self, "__time");
    else if (!strcmp(name, "getEventTimeNano")) r->j = jo_get_long(self, "__time") * 1000000LL;
    else if (!strcmp(name, "getDownTime")) r->j = jo_get_long(self, "__downtime");
    else if (!strcmp(name, "getKeyCode")) r->i = jo_get_int(self, "__keycode");
    else if (!strcmp(name, "getScanCode")) r->i = 0;
    else if (!strcmp(name, "getUnicodeChar")) r->i = 0;
    else if (!strcmp(name, "getDevice")) {
        static jobject device = NULL;
        if (!device) device = jo_new_object("android/view/InputDevice");
        r->l = device;
    }
    else if (!strcmp(name, "isFromSource")) r->z = (jo_get_int(self, "__source") & args[0].i) == args[0].i;
    else if (!strcmp(name, "recycle")) { /* retired with the others */ }
    else if (!strcmp(name, "getRepeatCount") || !strcmp(name, "getMetaState") || !strcmp(name, "getFlags") ||
             !strcmp(name, "getHistorySize") || !strcmp(name, "getButtonState") || !strcmp(name, "getEdgeFlags") ||
             !strcmp(name, "getModifiers") || !strcmp(name, "getDisplayId"))
        r->i = 0;
    else if (!strcmp(name, "getOrientation") || !strcmp(name, "getHistoricalX") || !strcmp(name, "getHistoricalY"))
        r->f = 0.0f;
    else
        return 0;
    return 1;
}
