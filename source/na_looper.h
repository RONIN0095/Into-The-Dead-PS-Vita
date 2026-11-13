/*
 * Copyright (C) 2026 Vita port contributors
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

/**
 * @file  na_looper.h
 * @brief Looper/Handler/Message/HandlerThread/Choreographer emulation.
 */

#ifndef NA_LOOPER_H
#define NA_LOOPER_H

#include <falso_jni/FalsoJNI.h>

/** Answers Java calls on loopers, handlers, messages and proxies. Returns 1 if handled. */
int na_looper_java_call(const char *cls, const char *name, const char *sig, jobject self,
                        const jvalue *args, int argc, jvalue *r);

/** Runs the main looper's due messages and frame callbacks (main loop). */
void na_looper_tick_main(void);

/** Calls interface method iface.method(sig) on a Java proxy backed by native code. */
jobject na_proxy_invoke(jobject proxy, const char *iface, const char *method, const char *sig,
                        jobject *args, int argc);

#endif // NA_LOOPER_H
