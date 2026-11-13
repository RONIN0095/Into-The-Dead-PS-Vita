/*
 * Copyright (C) 2026 Vita port contributors
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

/**
 * @file  na_unity.c
 * @brief Plays the part of Unity's Java player (com.unity3d.player.UnityPlayer, Unity 2020.3).
 *
 * On Android:
 *   1. libmain.so's JNI_OnLoad -> NativeLoader.load() -> libunity.so's
 *      JNI_OnLoad, which registers the UnityPlayer natives (RegisterNatives).
 *   2. The player calls initJni(Context), then nativeRecreateGfxState() when
 *      the surface exists, nativeResume(), nativeFocusChanged(true).
 *   3. Every frame (Choreographer) it calls nativeRender().
 *
 * The natives' signatures are logged when they are registered. Each call
 * checks the registered signature first and is skipped (with a log line) if
 * it differs from what this file expects, instead of calling with the wrong
 * arguments.
 */

#include "na_unity.h"

#include <stdlib.h>
#include <string.h>

#include <psp2/kernel/processmgr.h>

#include "jni_objects.h"
#include "na_android.h"
#include "na_modules.h"
#include "na_apk.h"
#include "utils/dialog.h"
#include "utils/logger.h"

#define MAX_NATIVES 256

typedef struct {
    char cls[96];
    char name[64];
    char sig[192];
    void *fn;
} native_entry;

static native_entry natives[MAX_NATIVES];
static int n_natives = 0;

void jo_register_natives(const char *cls, const JNINativeMethod *methods, int n) {
    for (int i = 0; i < n; i++) {
        if (n_natives >= MAX_NATIVES) {
            l_error("[natives] table full");
            return;
        }
        native_entry *e = &natives[n_natives++];
        strncpy(e->cls, cls, sizeof(e->cls) - 1);
        strncpy(e->name, methods[i].name, sizeof(e->name) - 1);
        strncpy(e->sig, methods[i].signature, sizeof(e->sig) - 1);
        e->fn = methods[i].fnPtr;
        l_info("[natives] %s.%s%s -> %p", cls, e->name, e->sig, e->fn);
    }
}

static native_entry *find_native(const char *name) {
    for (int i = 0; i < n_natives; i++)
        if (strcmp(natives[i].name, name) == 0) return &natives[i];
    return NULL;
}

// The native if its argument list matches; NULL (and a log line) otherwise.
void *na_unity_native(const char *name, const char *args);
static void *native_with_args(const char *name, const char *args) {
    native_entry *e = find_native(name);
    if (!e) {
        static int warned = 0;
        if (warned++ < 16) l_warn("[unity] %s is not registered", name);
        return NULL;
    }
    size_t n = strlen(args);
    if (strncmp(e->sig, args, n) != 0 || e->sig[n] != ')') {
        static int warned = 0;
        if (warned++ < 16) l_warn("[unity] %s has signature %s, expected %s)...: not called", name, e->sig, args);
        return NULL;
    }
    return e->fn;
}

static JNIEnv *env;
static jobject player;
static jobject activity;
static jobject surface;

void na_unity_start(void) {
    env = jo_env();

    // IL2CPP copies Managed/ from the APK to files/il2cpp/ on first start;
    // that copy fails here, so it is done up front
    na_apk_extract_prefix("assets/bin/Data/Managed/", DATA_PATH "files/il2cpp/");

    // libunity.so's constructors run when "libmain" loads it
    na_module_init(NA_LIB_UNITY);

    jint (*JNI_OnLoad)(void *vm, void *reserved) = (void *)na_module_symbol(NA_LIB_UNITY, "JNI_OnLoad");
    if (!JNI_OnLoad)
        fatal_error("libunity.so has no JNI_OnLoad.");
    l_info("[unity] JNI_OnLoad...");
    jint v = JNI_OnLoad(&jvm, NULL);
    l_info("[unity] JNI_OnLoad returned 0x%x, %d natives registered", v, n_natives);

    activity = jo_new_object("com/unity3d/player/UnityPlayerActivity");
    player = jo_new_object("com/unity3d/player/UnityPlayer");
    surface = jo_new_object("android/view/Surface");

    // Unity 2020.3: initJni(Context). (Unity 6 has initJni(Context, int).)
    void (*initJni)(JNIEnv *, jobject, jobject) = native_with_args("initJni", "(Landroid/content/Context;");
    if (initJni) {
        l_info("[unity] initJni...");
        initJni(env, player, activity);
        l_info("[unity] initJni done");
    }

    void (*recreate)(JNIEnv *, jobject, jint, jobject) = native_with_args("nativeRecreateGfxState", "(ILandroid/view/Surface;");
    if (recreate) {
        l_info("[unity] nativeRecreateGfxState...");
        recreate(env, player, 0, surface);
        l_info("[unity] nativeRecreateGfxState done");
    }

    void (*surface_changed)(JNIEnv *, jobject) = native_with_args("nativeSendSurfaceChangedEvent", "(");
    if (surface_changed) surface_changed(env, player);

    void (*set_running)(JNIEnv *, jobject, jboolean) = native_with_args("nativeUnityPlayerSetRunning", "(Z");
    if (set_running) set_running(env, player, JNI_TRUE);

    int (*resume)(JNIEnv *, jobject) = native_with_args("nativeResume", "(");
    if (resume) {
        l_info("[unity] nativeResume...");
        resume(env, player);
    }

    void (*focus)(JNIEnv *, jobject, jboolean) = native_with_args("nativeFocusChanged", "(Z");
    if (focus) focus(env, player, JNI_TRUE);

    l_info("[unity] started");
}

int na_unity_frame(void) {
    static int (*render)(JNIEnv *, jobject) = NULL;
    static int looked_up = 0;
    if (!looked_up) {
        looked_up = 1;
        render = native_with_args("nativeRender", "(");
        if (!render) fatal_error("nativeRender is not registered (see log.txt).");
    }
    na_choreographer_tick();
    return render(env, player);
}

void na_unity_inject_event(jobject ev) {
    static jboolean (*inject)(JNIEnv *, jobject, jobject) = NULL;
    static int looked_up = 0;
    if (!looked_up) {
        looked_up = 1;
        inject = native_with_args("nativeInjectEvent", "(Landroid/view/InputEvent;");
    }
    if (inject) inject(env, player, ev);
}

jobject na_unity_activity(void) {
    return activity;
}

jobject na_unity_player(void) {
    return player;
}

void *na_unity_native(const char *name, const char *args) { return native_with_args(name, args); }
