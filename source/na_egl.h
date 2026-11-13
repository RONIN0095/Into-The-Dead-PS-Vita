/*
 * Copyright (C) 2026 Vita port contributors
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

/**
 * @file  na_egl.h
 * @brief EGL as Unity 6 expects it: an ES 3.0 capable config and context.
 */

#ifndef NA_EGL_H
#define NA_EGL_H

#include <vitaGL.h>

EGLDisplay na_eglGetDisplay(NativeDisplayType d);
EGLBoolean na_eglInitialize(EGLDisplay dpy, EGLint *major, EGLint *minor);
EGLBoolean na_eglTerminate(EGLDisplay dpy);
EGLBoolean na_eglBindAPI(EGLenum api);
EGLBoolean na_eglChooseConfig(EGLDisplay dpy, const EGLint *attrib_list, EGLConfig *configs,
                              EGLint config_size, EGLint *num_config);
EGLBoolean na_eglGetConfigs(EGLDisplay dpy, EGLConfig *configs, EGLint config_size, EGLint *num_config);
EGLBoolean na_eglGetConfigAttrib(EGLDisplay dpy, EGLConfig config, EGLint attribute, EGLint *value);
EGLContext na_eglCreateContext(EGLDisplay dpy, EGLConfig config, EGLContext share, const EGLint *attrib_list);
EGLBoolean na_eglDestroyContext(EGLDisplay dpy, EGLContext ctx);
EGLSurface na_eglCreateWindowSurface(EGLDisplay dpy, EGLConfig config, void *win, const EGLint *attrib_list);
EGLSurface na_eglCreatePbufferSurface(EGLDisplay dpy, EGLConfig config, const EGLint *attrib_list);
EGLBoolean na_eglDestroySurface(EGLDisplay dpy, EGLSurface surface);
EGLBoolean na_eglMakeCurrent(EGLDisplay dpy, EGLSurface draw, EGLSurface read, EGLContext ctx);
EGLContext na_eglGetCurrentContext(void);
EGLSurface na_eglGetCurrentSurface(EGLint readdraw);
EGLDisplay na_eglGetCurrentDisplay(void);
EGLBoolean na_eglQuerySurface(EGLDisplay dpy, EGLSurface surface, EGLint attribute, EGLint *value);
EGLBoolean na_eglQueryContext(EGLDisplay dpy, EGLContext ctx, EGLint attribute, EGLint *value);
EGLBoolean na_eglSurfaceAttrib(EGLDisplay dpy, EGLSurface surface, EGLint attribute, EGLint value);
EGLBoolean na_eglSwapBuffers(EGLDisplay dpy, EGLSurface surface);
EGLBoolean na_eglSwapInterval(EGLDisplay dpy, EGLint interval);
EGLint na_eglGetError(void);
const char *na_eglQueryString(EGLDisplay dpy, EGLint name);
void *na_eglGetProcAddress(const char *name);

/** Number of eglSwapBuffers calls so far. */
extern volatile int na_egl_swap_count;

#endif // NA_EGL_H
