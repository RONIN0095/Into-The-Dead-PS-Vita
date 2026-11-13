/*
 * Copyright (C) 2026 Vita port contributors
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

/**
 * @file  na_egl.c
 * @brief EGL for Unity 6 on top of vitaGL.
 *
 * Unity 6 only runs on OpenGL ES 3.0+ ("[EGL] Checking ES 3.%d support...").
 * The config therefore reports EGL_OPENGL_ES3_BIT, contexts accept
 * EGL_CONTEXT_CLIENT_VERSION 3, and the extension list only names what is
 * really there (no fence sync, no frame timestamps).
 *
 * vitaGL has a single GL context. Every context Unity creates maps to it;
 * which thread makes which context current is logged, because a second
 * thread rendering at the same time would need a GL lock (see
 * docs/NOTES.md).
 */

#include "na_egl.h"

#include <stdio.h>
#include <string.h>

#include <psp2/kernel/threadmgr.h>

#include "na_android.h"
#include "utils/logger.h"

void *na_gl_get_proc(const char *name);

#define EGL_OPENGL_ES3_BIT_KHR 0x0040
#ifndef EGL_PBUFFER_BIT
#define EGL_PBUFFER_BIT 0x0001
#endif
#ifndef EGL_BAD_MATCH
#define EGL_BAD_MATCH 0x3009
#endif
#ifndef EGL_BAD_ALLOC
#define EGL_BAD_ALLOC 0x3003
#endif
#ifndef EGL_BUFFER_DESTROYED
#define EGL_BUFFER_DESTROYED 0x3095
#endif
#define EGL_CONTEXT_MAJOR_VERSION_KHR 0x3098 // same as EGL_CONTEXT_CLIENT_VERSION
#define EGL_CONTEXT_MINOR_VERSION_KHR 0x30FB
#define EGL_CONTEXT_FLAGS_KHR 0x30FC
#define EGL_RECORDABLE_ANDROID 0x3142
#define EGL_FRAMEBUFFER_TARGET_ANDROID 0x3147
#define EGL_CONFIG_SELECT_GROUP_ANDROID 0x34C0
#define EGL_COLOR_COMPONENT_TYPE_EXT 0x3339
#define EGL_COLOR_COMPONENT_TYPE_FIXED_EXT 0x333A

#define NA_DISPLAY ((EGLDisplay)1)
#define NA_CONFIG ((EGLConfig)1)
#define NA_WINDOW_SURFACE ((EGLSurface)1)
#define NA_PBUFFER_SURFACE ((EGLSurface)2)

#define MAX_CONTEXTS 4

static int contexts_used[MAX_CONTEXTS + 1];
static EGLint egl_error = EGL_SUCCESS;
static __thread EGLContext current_ctx = EGL_NO_CONTEXT;
static __thread EGLSurface current_surf = EGL_NO_SURFACE;
static int swap_interval = 1;
volatile int na_egl_swap_count = 0;

#define OK(v) do { egl_error = EGL_SUCCESS; return (v); } while (0)
#define FAIL(e, v) do { egl_error = (e); return (v); } while (0)

EGLDisplay na_eglGetDisplay(NativeDisplayType d) {
    OK(NA_DISPLAY);
}

EGLBoolean na_eglInitialize(EGLDisplay dpy, EGLint *major, EGLint *minor) {
    if (major) *major = 1;
    if (minor) *minor = 4;
    l_info("eglInitialize");
    OK(EGL_TRUE);
}

EGLBoolean na_eglTerminate(EGLDisplay dpy) {
    OK(EGL_TRUE);
}

EGLBoolean na_eglBindAPI(EGLenum api) {
    OK(api == EGL_OPENGL_ES_API ? EGL_TRUE : EGL_FALSE);
}

static void log_attribs(const char *what, const EGLint *a) {
    if (!a) return;
    char buf[512];
    int n = 0;
    for (; a[0] != EGL_NONE && n < (int)sizeof(buf) - 24; a += 2)
        n += snprintf(buf + n, sizeof(buf) - n, " %x=%d", a[0], a[1]);
    buf[n] = 0;
    l_info("%s:%s", what, buf);
}

EGLBoolean na_eglChooseConfig(EGLDisplay dpy, const EGLint *attrib_list, EGLConfig *configs,
                              EGLint config_size, EGLint *num_config) {
    log_attribs("eglChooseConfig", attrib_list);
    if (!num_config) FAIL(EGL_BAD_PARAMETER, EGL_FALSE);
    // Refuse multisampled configs: Unity then renders without MSAA.
    for (const EGLint *a = attrib_list; a && a[0] != EGL_NONE; a += 2) {
        if ((a[0] == EGL_SAMPLES && a[1] > 1) || (a[0] == EGL_SAMPLE_BUFFERS && a[1] > 0)) {
            *num_config = 0;
            OK(EGL_TRUE);
        }
    }
    *num_config = 1;
    if (configs && config_size > 0) configs[0] = NA_CONFIG;
    OK(EGL_TRUE);
}

EGLBoolean na_eglGetConfigs(EGLDisplay dpy, EGLConfig *configs, EGLint config_size, EGLint *num_config) {
    if (!num_config) FAIL(EGL_BAD_PARAMETER, EGL_FALSE);
    *num_config = 1;
    if (configs && config_size > 0) configs[0] = NA_CONFIG;
    OK(EGL_TRUE);
}

EGLBoolean na_eglGetConfigAttrib(EGLDisplay dpy, EGLConfig config, EGLint attribute, EGLint *value) {
    EGLint v = 0;
    switch (attribute) {
        case EGL_RED_SIZE: case EGL_GREEN_SIZE: case EGL_BLUE_SIZE: case EGL_ALPHA_SIZE:
            v = 8; break;
        case EGL_BUFFER_SIZE: v = 32; break;
        case EGL_DEPTH_SIZE: v = 24; break;
        case EGL_STENCIL_SIZE: v = 8; break;
        case EGL_SAMPLES: case EGL_SAMPLE_BUFFERS: v = 0; break;
        case EGL_RENDERABLE_TYPE:
        case EGL_CONFORMANT:
            v = EGL_OPENGL_ES_BIT | EGL_OPENGL_ES2_BIT | EGL_OPENGL_ES3_BIT_KHR; break;
        case EGL_SURFACE_TYPE: v = EGL_WINDOW_BIT | EGL_PBUFFER_BIT; break;
        case EGL_CONFIG_CAVEAT: v = EGL_NONE; break;
        case EGL_CONFIG_ID: v = 1; break;
        case EGL_COLOR_BUFFER_TYPE: v = EGL_RGB_BUFFER; break;
        case EGL_NATIVE_VISUAL_ID: v = 1; break; // WINDOW_FORMAT_RGBA_8888
        case EGL_MIN_SWAP_INTERVAL: v = 0; break;
        case EGL_MAX_SWAP_INTERVAL: v = 4; break;
        case EGL_MAX_PBUFFER_WIDTH: v = SCREEN_W; break;
        case EGL_MAX_PBUFFER_HEIGHT: v = SCREEN_H; break;
        case EGL_MAX_PBUFFER_PIXELS: v = SCREEN_W * SCREEN_H; break;
        case EGL_RECORDABLE_ANDROID: v = EGL_FALSE; break;
        case EGL_FRAMEBUFFER_TARGET_ANDROID: v = EGL_TRUE; break;
        case EGL_COLOR_COMPONENT_TYPE_EXT: v = EGL_COLOR_COMPONENT_TYPE_FIXED_EXT; break;
        case EGL_TRANSPARENT_TYPE: v = EGL_NONE; break;
        default:
            l_debug("eglGetConfigAttrib(0x%x): 0", attribute);
            v = 0;
            break;
    }
    if (value) *value = v;
    OK(EGL_TRUE);
}

EGLContext na_eglCreateContext(EGLDisplay dpy, EGLConfig config, EGLContext share, const EGLint *attrib_list) {
    int version = 1;
    for (const EGLint *a = attrib_list; a && a[0] != EGL_NONE; a += 2)
        if (a[0] == EGL_CONTEXT_CLIENT_VERSION) version = a[1];
    log_attribs("eglCreateContext", attrib_list);
    if (version > 3) {
        l_info("eglCreateContext: ES %d.x requested, not supported", version);
        FAIL(EGL_BAD_MATCH, EGL_NO_CONTEXT);
    }
    for (int i = 1; i <= MAX_CONTEXTS; i++) {
        if (!contexts_used[i]) {
            contexts_used[i] = 1;
            l_info("eglCreateContext: context %d (ES %d, shared with %p) on thread 0x%x",
                   i, version, share, sceKernelGetThreadId());
            OK((EGLContext)(uintptr_t)i);
        }
    }
    FAIL(EGL_BAD_ALLOC, EGL_NO_CONTEXT);
}

EGLBoolean na_eglDestroyContext(EGLDisplay dpy, EGLContext ctx) {
    uintptr_t i = (uintptr_t)ctx;
    if (i >= 1 && i <= MAX_CONTEXTS) contexts_used[i] = 0;
    l_info("eglDestroyContext(%d)", (int)i);
    OK(EGL_TRUE);
}

EGLSurface na_eglCreateWindowSurface(EGLDisplay dpy, EGLConfig config, void *win, const EGLint *attrib_list) {
    log_attribs("eglCreateWindowSurface", attrib_list);
    OK(NA_WINDOW_SURFACE);
}

EGLSurface na_eglCreatePbufferSurface(EGLDisplay dpy, EGLConfig config, const EGLint *attrib_list) {
    log_attribs("eglCreatePbufferSurface", attrib_list);
    OK(NA_PBUFFER_SURFACE);
}

EGLBoolean na_eglDestroySurface(EGLDisplay dpy, EGLSurface surface) {
    OK(EGL_TRUE);
}

EGLBoolean na_eglMakeCurrent(EGLDisplay dpy, EGLSurface draw, EGLSurface read, EGLContext ctx) {
    if (ctx != current_ctx)
        l_info("eglMakeCurrent(ctx %d, surface %d) on thread 0x%x",
               (int)(uintptr_t)ctx, (int)(uintptr_t)draw, sceKernelGetThreadId());
    current_ctx = ctx;
    current_surf = draw;
    OK(EGL_TRUE);
}

EGLContext na_eglGetCurrentContext(void) { return current_ctx; }
EGLSurface na_eglGetCurrentSurface(EGLint readdraw) { return current_surf; }
EGLDisplay na_eglGetCurrentDisplay(void) { return current_ctx ? NA_DISPLAY : EGL_NO_DISPLAY; }

EGLBoolean na_eglQuerySurface(EGLDisplay dpy, EGLSurface surface, EGLint attribute, EGLint *value) {
    EGLint v = 0;
    switch (attribute) {
        case EGL_WIDTH: v = SCREEN_W; break;
        case EGL_HEIGHT: v = SCREEN_H; break;
        case EGL_CONFIG_ID: v = 1; break;
        case EGL_RENDER_BUFFER: v = EGL_BACK_BUFFER; break;
        case EGL_SWAP_BEHAVIOR: v = EGL_BUFFER_DESTROYED; break;
        default:
            l_debug("eglQuerySurface(0x%x): 0", attribute);
            break;
    }
    if (value) *value = v;
    OK(EGL_TRUE);
}

EGLBoolean na_eglQueryContext(EGLDisplay dpy, EGLContext ctx, EGLint attribute, EGLint *value) {
    EGLint v = 0;
    switch (attribute) {
        case EGL_CONFIG_ID: v = 1; break;
        case EGL_CONTEXT_CLIENT_TYPE: v = EGL_OPENGL_ES_API; break;
        case EGL_CONTEXT_CLIENT_VERSION: v = 3; break;
        case EGL_RENDER_BUFFER: v = EGL_BACK_BUFFER; break;
        default: break;
    }
    if (value) *value = v;
    OK(EGL_TRUE);
}

EGLBoolean na_eglSurfaceAttrib(EGLDisplay dpy, EGLSurface surface, EGLint attribute, EGLint value) {
    l_debug("eglSurfaceAttrib(0x%x, %d)", attribute, value);
    OK(EGL_TRUE);
}

void na_gl_screenshot(int n); // na_gles3.c
#include "na_stats.h"
#include "na_input.h"
#include <psp2/kernel/processmgr.h>

EGLBoolean na_eglSwapBuffers(EGLDisplay dpy, EGLSurface surface) {
    // Debug screenshots: 5 s and 20 s into the game
    // (automatic screenshots at swaps 150/600 are off: each took seconds; L+R+DOWN still saves one)
    // frame trace requested with L+R+DOWN: this frame's draws were logged
    extern volatile int na_trace_swap;
    extern volatile int na_trace_request;
    if (na_egl_swap_count == na_trace_swap) {
        na_gl_screenshot(na_egl_swap_count);
        na_trace_swap = -1;
    } else if (na_trace_request && na_trace_swap < 0) {
        na_trace_request = 0;
        void na_gl_dump_recent_draws(void);
        na_gl_dump_recent_draws();
        na_trace_swap = na_egl_swap_count + 1;
        l_info("[GL] tracing frame %d (L+R+DOWN)", na_trace_swap);
    }
    na_input_draw_cursor();
    uint64_t t0 = sceKernelGetProcessTimeWide();
    vglSwapBuffers(GL_FALSE);
    na_stats.swap_us += (uint32_t)(sceKernelGetProcessTimeWide() - t0);
    na_egl_swap_count++;
    OK(EGL_TRUE);
}

EGLBoolean na_eglSwapInterval(EGLDisplay dpy, EGLint interval) {
    if (interval != swap_interval) {
        swap_interval = interval;
        vglWaitVblankStart(interval > 0 ? GL_TRUE : GL_FALSE);
    }
    OK(EGL_TRUE);
}

EGLint na_eglGetError(void) {
    EGLint e = egl_error;
    egl_error = EGL_SUCCESS;
    return e;
}

const char *na_eglQueryString(EGLDisplay dpy, EGLint name) {
    switch (name) {
        case EGL_VENDOR: OK("vitaGL");
        case EGL_VERSION: OK("1.4 vitaGL");
        case EGL_CLIENT_APIS: OK("OpenGL_ES");
        case EGL_EXTENSIONS: OK("EGL_KHR_create_context EGL_KHR_surfaceless_context");
        default: FAIL(EGL_BAD_PARAMETER, NULL);
    }
}

void *na_eglGetProcAddress(const char *name) {
    void *p = na_gl_get_proc(name);
    if (!p) l_info("eglGetProcAddress(%s): NULL", name);
    return p;
}
