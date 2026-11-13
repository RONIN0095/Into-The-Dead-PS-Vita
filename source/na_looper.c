/*
 * Copyright (C) 2026 Vita port contributors
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

/**
 * @file  na_looper.c
 * @brief android.os.Looper/Handler/Message/HandlerThread and
 *        android.view.Choreographer for the Java side of Unity.
 *
 * Unity 6 moves parts of its main loop onto a HandlerThread: it posts
 * messages to a Handler whose Callback is a Java proxy for native code
 * (JNIBridge.newInterfaceProxy), and registers Choreographer frame
 * callbacks the same way. Each looper here is a real thread with a message
 * queue; proxies are called through ReflectionHelper.nativeProxyInvoke.
 * The main looper is serviced from the main loop (na_looper_tick_main).
 */

#include "na_looper.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>

#include "jni_objects.h"
#include "utils/logger.h"

void *na_unity_native(const char *name, const char *args); // na_unity.c

#define MAX_LOOPERS 8
#define MAX_MSGS 256
#define MAX_FRAME_CBS 16

typedef struct {
    jobject handler;
    jobject msg;       // Message, or NULL for a Runnable
    jobject runnable;
    uint64_t when_us;
    int used;
} queued;

typedef struct {
    jobject looper_obj;
    SceUID thread;     // 0 for the main looper
    SceUID sema;
    SceKernelLwMutexWork lock;
    queued q[MAX_MSGS];
    jobject frame_cbs[MAX_FRAME_CBS];
    int n_frame_cbs;
    uint64_t next_frame_us;
    volatile int quit;
} looper;

static looper loopers[MAX_LOOPERS];
static int n_loopers = 0;
static SceKernelLwMutexWork global_lock;
static int ready = 0;

static __thread looper *current_looper = NULL;

static void init_once(void) {
    if (ready) return;
    sceKernelCreateLwMutex(&global_lock, "loopers", SCE_KERNEL_MUTEX_ATTR_RECURSIVE, 0, NULL);
    ready = 1;
}

static uint64_t now_us(void) { return sceKernelGetProcessTimeWide(); }

static looper *new_looper(void) {
    init_once();
    sceKernelLockLwMutex(&global_lock, 1, NULL);
    looper *l = n_loopers < MAX_LOOPERS ? &loopers[n_loopers++] : NULL;
    sceKernelUnlockLwMutex(&global_lock, 1);
    if (!l) {
        l_error("[looper] too many loopers");
        return NULL;
    }
    memset(l, 0, sizeof(*l));
    sceKernelCreateLwMutex(&l->lock, "looper", SCE_KERNEL_MUTEX_ATTR_RECURSIVE, 0, NULL);
    l->sema = sceKernelCreateSema("looper", 0, 0, 1000, NULL);
    l->looper_obj = jo_new_object("android/os/Looper");
    jo_set_long(l->looper_obj, "__looper", (jlong)(uintptr_t)l);
    return l;
}

static looper *looper_of(jobject obj) {
    if (!obj || !jo_is_object(obj)) return NULL;
    return (looper *)(uintptr_t)jo_get_long(obj, "__looper");
}

static looper *main_looper(void) {
    static looper *m = NULL;
    if (!m) m = new_looper();
    return m;
}

/*
 * Calling Java proxies (native code behind JNIBridge/ReflectionHelper)
 *
 * JNIBridge.newInterfaceProxy(ptr, Class[]) proxies (Unity's C++ and C#
 * AndroidJavaProxy) are called through JNIBridge.invoke(ptr, Class, Method,
 * Object[]); the native side gets the jmethodID with FromReflectedMethod.
 * ReflectionHelper proxies go through nativeProxyInvoke(ptr, name, Object[]).
 */
#define PROXY_JNIBRIDGE 0
#define PROXY_REFLECTION 1

static jobject make_args(JNIEnv *env, jobject *args, int argc) {
    jclass objc = (*env)->FindClass(env, "java/lang/Object");
    jobjectArray arr = (*env)->NewObjectArray(env, argc, objc, NULL);
    for (int i = 0; i < argc; i++) (*env)->SetObjectArrayElement(env, arr, i, args[i]);
    return arr;
}

jobject na_proxy_invoke(jobject proxy, const char *iface, const char *method, const char *sig,
                        jobject *args, int argc) {
    if (!proxy || !jo_is_object(proxy) || strcmp(jo_class_name(proxy), "java/lang/reflect/Proxy") != 0) {
        l_warn("[looper] %s.%s on a non-proxy object %p", iface, method, proxy);
        return NULL;
    }
    JNIEnv *env = jo_env();
    jlong ptr = jo_get_long(proxy, "__ptr");

    if (jo_get_int(proxy, "__kind") == PROXY_REFLECTION) {
        static jobject (*invoke)(JNIEnv *, jclass, jlong, jstring, jobjectArray) = NULL;
        if (!invoke) invoke = na_unity_native("nativeProxyInvoke", "(JLjava/lang/String;[Ljava/lang/Object;");
        if (!invoke) return NULL;
        jclass helper = (*env)->FindClass(env, "com/unity3d/player/ReflectionHelper");
        return invoke(env, helper, ptr, (*env)->NewStringUTF(env, method), make_args(env, args, argc));
    }

    static jobject (*invoke)(JNIEnv *, jclass, jlong, jclass, jobject, jobjectArray) = NULL;
    if (!invoke) invoke = na_unity_native("invoke", "(JLjava/lang/Class;Ljava/lang/reflect/Method;[Ljava/lang/Object;");
    if (!invoke) return NULL;
    // the interface the proxy was made for, if it is the one we are calling
    jclass cls = (jclass)jo_get_obj(proxy, "__iface");
    if (!cls || strcmp((const char *)cls, iface) != 0) cls = (*env)->FindClass(env, iface);
    // Method objects are cached: one per interface method (they were leaking one per frame)
    static struct { jclass cls; const char *name; jobject m; } methods[32];
    static int n_methods = 0;
    jobject m = NULL;
    for (int i = 0; i < n_methods; i++)
        if (methods[i].cls == cls && strcmp(methods[i].name, method) == 0) m = methods[i].m;
    if (!m) {
        m = jo_new_object("java/lang/reflect/Method");
        jo_set_long(m, "__mid", (jlong)(uintptr_t)(*env)->GetMethodID(env, cls, method, sig));
        jo_set_string(m, "__name", method);
        jo_set_obj(m, "__class", (jobject)cls);
        if (n_methods < 32) methods[n_methods++] = (typeof(methods[0])){ cls, method, m };
    }
    jclass bridge = (*env)->FindClass(env, "bitter/jnibridge/JNIBridge");
    return invoke(env, bridge, ptr, cls, m, make_args(env, args, argc));
}

static jobject new_proxy(jlong ptr, int kind, jobject iface) {
    jobject p = jo_new_object("java/lang/reflect/Proxy");
    jo_set_long(p, "__ptr", ptr);
    jo_set_int(p, "__kind", kind);
    jo_set_obj(p, "__iface", iface);
    return p;
}

static jobject boxed_long(jlong v) {
    jobject o = jo_new_object("java/lang/Long");
    jo_set_long(o, "value", v);
    return o;
}

/*
 * Message queue
 */
static void enqueue(looper *l, jobject handler, jobject msg, jobject runnable, uint64_t delay_us) {
    if (!l) return;
    sceKernelLockLwMutex(&l->lock, 1, NULL);
    for (int i = 0; i < MAX_MSGS; i++) {
        if (!l->q[i].used) {
            l->q[i] = (queued){ handler, msg, runnable, now_us() + delay_us, 1 };
            sceKernelUnlockLwMutex(&l->lock, 1);
            sceKernelSignalSema(l->sema, 1);
            return;
        }
    }
    sceKernelUnlockLwMutex(&l->lock, 1);
    l_error("[looper] queue full");
}

static void dispatch(queued *m) {
    static int logged = 0;
    if (logged++ < 8) {
        jobject cb = m->runnable ? m->runnable : (m->handler ? jo_get_obj(m->handler, "__callback") : NULL);
        l_info("[looper] dispatch what=%d target=%s", m->msg ? jo_get_int(m->msg, "what") : -1,
               cb && jo_is_object(cb) ? jo_class_name(cb) : (cb ? "(foreign object)" : "(none)"));
    }
    if (m->runnable) {
        na_proxy_invoke(m->runnable, "java/lang/Runnable", "run", "()V", NULL, 0);
        return;
    }
    jobject cb = m->handler ? jo_get_obj(m->handler, "__callback") : NULL;
    if (cb) {
        jobject a[1] = { m->msg };
        na_proxy_invoke(cb, "android/os/Handler$Callback", "handleMessage", "(Landroid/os/Message;)Z", a, 1);
    } else {
        // Message.callback (a Runnable posted with the message)
        jobject r = m->msg ? jo_get_obj(m->msg, "callback") : NULL;
        if (r) na_proxy_invoke(r, "java/lang/Runnable", "run", "()V", NULL, 0);
    }
    // Messages are recycled once handled, as on Android
    if (m->msg && jo_is_object(m->msg)) jo_free_object(m->msg);
}

// Runs due messages and frame callbacks. Returns microseconds until the next event.
static uint64_t service(looper *l) {
    uint64_t t = now_us(), next = 16000;
    for (;;) {
        queued m;
        int found = 0;
        sceKernelLockLwMutex(&l->lock, 1, NULL);
        for (int i = 0; i < MAX_MSGS; i++) {
            if (l->q[i].used && l->q[i].when_us <= t && (!found || l->q[i].when_us < m.when_us)) {
                m = l->q[i];
                found = i + 1;
            }
        }
        if (found) l->q[found - 1].used = 0;
        for (int i = 0; i < MAX_MSGS; i++)
            if (l->q[i].used && l->q[i].when_us > t && l->q[i].when_us - t < next) next = l->q[i].when_us - t;
        sceKernelUnlockLwMutex(&l->lock, 1);
        if (!found) break;
        dispatch(&m);
        t = now_us();
    }

    if (l->n_frame_cbs && t >= l->next_frame_us) {
        jobject cbs[MAX_FRAME_CBS];
        sceKernelLockLwMutex(&l->lock, 1, NULL);
        int n = l->n_frame_cbs;
        memcpy(cbs, l->frame_cbs, sizeof(jobject) * n);
        l->n_frame_cbs = 0;
        sceKernelUnlockLwMutex(&l->lock, 1);
        l->next_frame_us = t + 16666;
        jobject a[1] = { boxed_long((jlong)t * 1000) };
        for (int i = 0; i < n; i++) na_proxy_invoke(cbs[i], "android/view/Choreographer$FrameCallback", "doFrame", "(J)V", a, 1);
        jo_free_object(a[0]);
    }
    if (l->n_frame_cbs) {
        uint64_t f = l->next_frame_us > t ? l->next_frame_us - t : 0;
        if (f < next) next = f;
    }
    return next;
}

static int looper_thread(SceSize args, void *argp) {
    looper *l = *(looper **)argp;
    current_looper = l;
    l_info("[looper] thread running");
    while (!l->quit) {
        uint64_t wait = service(l);
        SceUInt to = (SceUInt)(wait > 100000 ? 100000 : wait);
        if (to) sceKernelWaitSema(l->sema, 1, &to);
    }
    return 0;
}

void na_looper_tick_main(void) {
    looper *m = main_looper();
    current_looper = m;
    service(m);
}

/*
 * Java methods (called from jo_java_call)
 */
static int is(const char *a, const char *b) { return strcmp(a, b) == 0; }

static jobject get_message(jobject handler, const char *sig, const jvalue *args, int argc) {
    jobject msg = jo_new_object("android/os/Message");
    jo_set_obj(msg, "target", handler);
    // obtainMessage(), (I), (ILjava/lang/Object;), (III), (IIILjava/lang/Object;)
    if (argc >= 1) jo_set_int(msg, "what", args[0].i);
    if (argc == 2) jo_set_obj(msg, "obj", args[1].l);
    if (argc >= 3) {
        jo_set_int(msg, "arg1", args[1].i);
        jo_set_int(msg, "arg2", args[2].i);
    }
    if (argc >= 4) jo_set_obj(msg, "obj", args[3].l);
    return msg;
}

int na_looper_java_call(const char *cls, const char *name, const char *sig, jobject self,
                        const jvalue *args, int argc, jvalue *r) {
    const char *sc = (self && jo_is_object(self)) ? jo_class_name(self) : "";

    if (is(name, "newInterfaceProxy") && argc >= 1) {
        // newInterfaceProxy(long ptr, Class[] interfaces)
        JNIEnv *env = jo_env();
        jobject iface = NULL;
        if (argc >= 2 && args[1].l && (*env)->GetArrayLength(env, args[1].l) > 0)
            iface = (*env)->GetObjectArrayElement(env, args[1].l, 0);
        r->l = new_proxy(args[0].j, PROXY_JNIBRIDGE, iface);
        l_info("[looper] newInterfaceProxy(%s) -> %p", iface ? (const char *)iface : "?", r->l);
        return 1;
    }
    if (is(name, "disableInterfaceProxy")) return 1;
    if ((is(name, "newProxyInstance") || is(name, "createProxy")) && argc >= 1 && sig[1] == 'J') {
        r->l = new_proxy(args[0].j, PROXY_REFLECTION, NULL);
        l_info("[looper] %s.%s -> proxy %p", cls, name, r->l);
        return 1;
    }

    // HandlerThread: new HandlerThread(name), start(), getLooper(), quit()
    if (strstr(sc, "HandlerThread") || strstr(cls, "HandlerThread")) {
        if (is(name, "start")) {
            looper *l = new_looper();
            if (!l) return 1;
            jo_set_long(self, "__looper", (jlong)(uintptr_t)l);
            l->thread = sceKernelCreateThread("HandlerThread", looper_thread, 0x10000100, 0x40000, 0, 0, NULL);
            sceKernelStartThread(l->thread, sizeof(l), &l);
            return 1;
        }
        if (is(name, "getLooper")) {
            looper *l = looper_of(self);
            r->l = l ? l->looper_obj : main_looper()->looper_obj;
            return 1;
        }
        if (is(name, "quit") || is(name, "quitSafely")) {
            looper *l = looper_of(self);
            if (l) l->quit = 1;
            r->z = JNI_TRUE;
            return 1;
        }
        if (is(name, "<init>") || is(name, "setPriority") || is(name, "setName")) return 1;
    }

    // Looper
    if (is(cls, "android/os/Looper") || is(sc, "android/os/Looper")) {
        if (is(name, "getMainLooper")) { r->l = main_looper()->looper_obj; return 1; }
        if (is(name, "myLooper")) { r->l = current_looper ? current_looper->looper_obj : main_looper()->looper_obj; return 1; }
        if (is(name, "prepare") || is(name, "loop") || is(name, "quit") || is(name, "quitSafely")) return 1;
        if (is(name, "getThread")) { r->l = jo_new_object("java/lang/Thread"); return 1; }
        if (is(name, "isCurrentThread")) { r->z = looper_of(self) == current_looper; return 1; }
    }

    // Handler
    if (is(cls, "android/os/Handler") || strstr(sc, "android/os/Handler")) {
        if (is(name, "<init>")) {
            // Handler(), (Looper), (Callback), (Looper, Callback)
            looper *l = current_looper ? current_looper : main_looper();
            jobject cb = NULL;
            for (int i = 0; i < argc; i++) {
                if (looper_of(args[i].l)) l = looper_of(args[i].l);
                else if (args[i].l) cb = args[i].l;
            }
            jo_set_long(self, "__looper", (jlong)(uintptr_t)l);
            jo_set_obj(self, "__callback", cb);
            return 1;
        }
        looper *l = looper_of(self);
        if (is(name, "obtainMessage")) { r->l = get_message(self, sig, args, argc); return 1; }
        if (is(name, "sendMessage") || is(name, "sendMessageAtFrontOfQueue")) {
            enqueue(l, self, args[0].l, NULL, 0); r->z = JNI_TRUE; return 1;
        }
        if (is(name, "sendMessageDelayed")) {
            enqueue(l, self, args[0].l, NULL, (uint64_t)args[1].j * 1000); r->z = JNI_TRUE; return 1;
        }
        if (is(name, "sendEmptyMessage") || is(name, "sendEmptyMessageDelayed")) {
            jvalue a[1] = { { .i = args[0].i } };
            jobject msg = get_message(self, "(I)", a, 1);
            enqueue(l, self, msg, NULL, argc >= 2 ? (uint64_t)args[1].j * 1000 : 0);
            r->z = JNI_TRUE;
            return 1;
        }
        if (is(name, "post") || is(name, "postAtFrontOfQueue")) {
            enqueue(l, self, NULL, args[0].l, 0); r->z = JNI_TRUE; return 1;
        }
        if (is(name, "postDelayed")) {
            enqueue(l, self, NULL, args[0].l, (uint64_t)args[1].j * 1000); r->z = JNI_TRUE; return 1;
        }
        if (is(name, "removeMessages") || is(name, "removeCallbacks") || is(name, "removeCallbacksAndMessages")) {
            if (l) {
                sceKernelLockLwMutex(&l->lock, 1, NULL);
                for (int i = 0; i < MAX_MSGS; i++) {
                    if (!l->q[i].used || l->q[i].handler != self) continue;
                    if (is(name, "removeCallbacksAndMessages") ||
                        (is(name, "removeMessages") && l->q[i].msg && jo_get_int(l->q[i].msg, "what") == args[0].i) ||
                        (is(name, "removeCallbacks") && l->q[i].runnable == args[0].l))
                        l->q[i].used = 0;
                }
                sceKernelUnlockLwMutex(&l->lock, 1);
            }
            return 1;
        }
        if (is(name, "getLooper")) { r->l = l ? l->looper_obj : NULL; return 1; }
    }

    // Message
    if (is(sc, "android/os/Message") || is(cls, "android/os/Message")) {
        if (is(name, "sendToTarget")) {
            jobject h = jo_get_obj(self, "target");
            enqueue(looper_of(h), h, self, NULL, 0);
            return 1;
        }
        if (is(name, "obtain")) {
            jobject msg = jo_new_object("android/os/Message");
            if (argc >= 1 && jo_is_object(args[0].l) && strstr(jo_class_name(args[0].l), "Handler"))
                jo_set_obj(msg, "target", args[0].l);
            if (argc >= 2) jo_set_int(msg, "what", args[1].i);
            r->l = msg;
            return 1;
        }
        if (is(name, "recycle")) return 1;
        if (is(name, "getTarget")) { r->l = jo_get_obj(self, "target"); return 1; }
    }

    // Choreographer
    if (is(cls, "android/view/Choreographer") || is(sc, "android/view/Choreographer")) {
        if (is(name, "getInstance")) {
            looper *l = current_looper ? current_looper : main_looper();
            jobject c = jo_new_object("android/view/Choreographer");
            jo_set_long(c, "__looper", (jlong)(uintptr_t)l);
            r->l = c;
            return 1;
        }
        looper *l = looper_of(self);
        if (!l) l = main_looper();
        if (is(name, "postFrameCallback") || is(name, "postFrameCallbackDelayed")) {
            sceKernelLockLwMutex(&l->lock, 1, NULL);
            if (l->n_frame_cbs < MAX_FRAME_CBS) l->frame_cbs[l->n_frame_cbs++] = args[0].l;
            sceKernelUnlockLwMutex(&l->lock, 1);
            sceKernelSignalSema(l->sema, 1);
            return 1;
        }
        if (is(name, "removeFrameCallback")) {
            sceKernelLockLwMutex(&l->lock, 1, NULL);
            for (int i = 0; i < l->n_frame_cbs; i++)
                if (l->frame_cbs[i] == args[0].l) l->frame_cbs[i--] = l->frame_cbs[--l->n_frame_cbs];
            sceKernelUnlockLwMutex(&l->lock, 1);
            return 1;
        }
    }

    // java.lang.reflect.Method made for JNIBridge.invoke
    if (is(sc, "java/lang/reflect/Method")) {
        if (is(name, "getName")) { r->l = jo_get_obj(self, "__name"); return 1; }
        if (is(name, "getDeclaringClass")) { r->l = jo_get_obj(self, "__class"); return 1; }
        if (is(name, "hashCode")) { r->i = (jint)(uintptr_t)jo_get_long(self, "__mid"); return 1; }
    }
    if (is(sc, "java/lang/reflect/Proxy")) {
        if (is(name, "hashCode")) { r->i = (jint)jo_get_long(self, "__ptr"); return 1; }
        if (is(name, "equals")) { r->z = argc >= 1 && args[0].l == self; return 1; }
    }

    // Boxed values passed to proxies
    if (is(name, "longValue") && strstr(sc, "Long")) { r->j = jo_get_long(self, "value"); return 1; }

    return 0;
}
