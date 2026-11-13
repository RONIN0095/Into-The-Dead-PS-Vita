/*
 * Copyright (C) 2026 Vita port contributors
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

/**
 * @file  jni_objects.c
 * @brief Minimal per-object Java emulation on top of FalsoJNI.
 *
 * FalsoJNI keeps providing strings and arrays. Classes, objects, instance
 * fields and method calls are replaced here: objects get real per-instance
 * fields, and every method call is routed to jo_java_call(), which the port
 * implements to stand in for the game's Java code.
 */

#include "jni_objects.h"

#include <stdarg.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <psp2/kernel/threadmgr.h>

#include "utils/logger.h"

#define DJ_MAX_FIELDS 24
#define DJ_OBJSET_SIZE 8192 // must be a power of two

typedef struct DJClass {
    char name[128]; // must stay first: FalsoJNI treats jclass as char*
    struct DJClass *next;
} DJClass;

typedef struct DJField {
    char name[64];
    char sig[64];
    struct DJField *next;
} DJField;

typedef struct DJMethod {
    DJClass *cls;
    char name[64];
    char sig[128];
    struct DJMethod *next;
} DJMethod;

typedef struct DJObject {
    DJClass *cls;
    int nfields;
    struct {
        DJField *f;
        jvalue v;
    } fields[DJ_MAX_FIELDS];
} DJObject;

static DJClass *classes = NULL;
static DJField *fields = NULL;
static DJMethod *methods = NULL;

static SceKernelLwMutexWork jo_lock;

// Open-addressing set of live DJObject pointers, so that we can tell our
// objects apart from FalsoJNI strings/arrays without dereferencing anything.
static uintptr_t objset[DJ_OBJSET_SIZE];
static int objset_count = 0;

static inline uint32_t ptr_hash(uintptr_t p) {
    return (uint32_t)((p >> 3) * 2654435761u) & (DJ_OBJSET_SIZE - 1);
}

static void objset_add(uintptr_t p) {
    if (objset_count >= DJ_OBJSET_SIZE * 3 / 4) {
        static int reported = 0;
        if (!reported++) {
            // Which classes fill the set: names the leak
            DJClass *cls[32] = { 0 };
            int counts[32] = { 0 }, n = 0;
            for (int i = 0; i < DJ_OBJSET_SIZE; i++) {
                if (objset[i] <= 1) continue;
                DJClass *c = ((DJObject *)objset[i])->cls;
                int k = 0;
                while (k < n && cls[k] != c) k++;
                if (k == n && n < 32) cls[n++] = c;
                if (k < n) counts[k]++;
            }
            for (int k = 0; k < n; k++)
                if (counts[k] > 50) l_error("[JO] live objects: %d x %s", counts[k], cls[k] ? cls[k]->name : "?");
        }
        if (reported < 8) l_error("[JO] Object set is full, leaking object tracking for %p", (void *)p);
        return;
    }
    uint32_t i = ptr_hash(p);
    while (objset[i] != 0 && objset[i] != 1)
        i = (i + 1) & (DJ_OBJSET_SIZE - 1);
    objset[i] = p;
    objset_count++;
}

static int objset_find(uintptr_t p) {
    uint32_t i = ptr_hash(p);
    for (int n = 0; n < DJ_OBJSET_SIZE; n++) {
        if (objset[i] == 0) return -1;
        if (objset[i] == p) return (int)i;
        i = (i + 1) & (DJ_OBJSET_SIZE - 1);
    }
    return -1;
}

static void objset_remove(uintptr_t p) {
    int i = objset_find(p);
    if (i >= 0) {
        objset[i] = 1; // tombstone
        objset_count--;
    }
}

/*
 * Interning
 */

static DJClass *intern_class(const char *name) {
    for (DJClass *c = classes; c; c = c->next)
        if (strcmp(c->name, name) == 0) return c;
    DJClass *c = calloc(1, sizeof(DJClass));
    strncpy(c->name, name, sizeof(c->name) - 1);
    c->next = classes;
    classes = c;
    return c;
}

// Fields are interned by name only: Java-side helpers do not know signatures,
// and storage lives in the object anyway.
static DJField *intern_field(const char *name, const char *sig) {
    for (DJField *f = fields; f; f = f->next)
        if (strcmp(f->name, name) == 0) return f;
    DJField *f = calloc(1, sizeof(DJField));
    strncpy(f->name, name, sizeof(f->name) - 1);
    if (sig) strncpy(f->sig, sig, sizeof(f->sig) - 1);
    f->next = fields;
    fields = f;
    return f;
}

static DJMethod *intern_method(DJClass *cls, const char *name, const char *sig) {
    for (DJMethod *m = methods; m; m = m->next)
        if (m->cls == cls && strcmp(m->name, name) == 0 && strcmp(m->sig, sig) == 0)
            return m;
    DJMethod *m = calloc(1, sizeof(DJMethod));
    m->cls = cls;
    strncpy(m->name, name, sizeof(m->name) - 1);
    strncpy(m->sig, sig, sizeof(m->sig) - 1);
    m->next = methods;
    methods = m;
    return m;
}

/*
 * Objects
 */

int jo_is_object(jobject obj) {
    if (!obj) return 0;
    sceKernelLockLwMutex(&jo_lock, 1, NULL);
    int r = objset_find((uintptr_t)obj) >= 0;
    sceKernelUnlockLwMutex(&jo_lock, 1);
    return r;
}

static jobject new_object_of(DJClass *cls) {
    DJObject *o = calloc(1, sizeof(DJObject));
    o->cls = cls;
    sceKernelLockLwMutex(&jo_lock, 1, NULL);
    objset_add((uintptr_t)o);
    sceKernelUnlockLwMutex(&jo_lock, 1);
    return (jobject)o;
}

jobject jo_new_object(const char *class_name) {
    sceKernelLockLwMutex(&jo_lock, 1, NULL);
    DJClass *c = intern_class(class_name);
    sceKernelUnlockLwMutex(&jo_lock, 1);
    return new_object_of(c);
}

jobject jo_clone_object(jobject obj) {
    if (!jo_is_object(obj)) return NULL;
    DJObject *o = calloc(1, sizeof(DJObject));
    memcpy(o, (DJObject *)obj, sizeof(DJObject));
    sceKernelLockLwMutex(&jo_lock, 1, NULL);
    objset_add((uintptr_t)o);
    sceKernelUnlockLwMutex(&jo_lock, 1);
    return (jobject)o;
}

void jo_free_object(jobject obj) {
    if (!jo_is_object(obj)) return;
    sceKernelLockLwMutex(&jo_lock, 1, NULL);
    objset_remove((uintptr_t)obj);
    sceKernelUnlockLwMutex(&jo_lock, 1);
    free(obj);
}

const char *jo_class_name(jobject obj) {
    if (!jo_is_object(obj)) return "<not an object>";
    return ((DJObject *)obj)->cls->name;
}

static jvalue *field_slot(jobject obj, DJField *f, int create) {
    if (!jo_is_object(obj)) {
        l_warn("[JO] Field access '%s' on non-object %p", f ? f->name : "?", obj);
        return NULL;
    }
    DJObject *o = (DJObject *)obj;
    for (int i = 0; i < o->nfields; i++)
        if (o->fields[i].f == f) return &o->fields[i].v;
    if (!create) return NULL;
    if (o->nfields >= DJ_MAX_FIELDS) {
        l_error("[JO] Too many fields on %s", o->cls->name);
        return NULL;
    }
    o->fields[o->nfields].f = f;
    memset(&o->fields[o->nfields].v, 0, sizeof(jvalue));
    return &o->fields[o->nfields++].v;
}

static jvalue field_get(jobject obj, jfieldID fid) {
    jvalue *v = field_slot(obj, (DJField *)fid, 0);
    if (v) return *v;
    jvalue zero;
    memset(&zero, 0, sizeof(zero));
    return zero;
}

static void field_set(jobject obj, jfieldID fid, jvalue val) {
    jvalue *v = field_slot(obj, (DJField *)fid, 1);
    if (v) *v = val;
}

static DJField *field_by_name(const char *name) {
    sceKernelLockLwMutex(&jo_lock, 1, NULL);
    DJField *f = intern_field(name, NULL);
    sceKernelUnlockLwMutex(&jo_lock, 1);
    return f;
}

jint jo_get_int(jobject obj, const char *n) { return field_get(obj, (jfieldID)field_by_name(n)).i; }
jlong jo_get_long(jobject obj, const char *n) { return field_get(obj, (jfieldID)field_by_name(n)).j; }
jfloat jo_get_float(jobject obj, const char *n) { return field_get(obj, (jfieldID)field_by_name(n)).f; }
jboolean jo_get_bool(jobject obj, const char *n) { return field_get(obj, (jfieldID)field_by_name(n)).z; }
jobject jo_get_obj(jobject obj, const char *n) { return field_get(obj, (jfieldID)field_by_name(n)).l; }

char *jo_get_string(jobject obj, const char *n) {
    return jo_jstring_to_utf8((jstring)jo_get_obj(obj, n));
}

void jo_set_int(jobject obj, const char *n, jint v) { jvalue x; x.j = 0; x.i = v; field_set(obj, (jfieldID)field_by_name(n), x); }
void jo_set_long(jobject obj, const char *n, jlong v) { jvalue x; x.j = v; field_set(obj, (jfieldID)field_by_name(n), x); }
void jo_set_float(jobject obj, const char *n, jfloat v) { jvalue x; x.j = 0; x.f = v; field_set(obj, (jfieldID)field_by_name(n), x); }
void jo_set_bool(jobject obj, const char *n, jboolean v) { jvalue x; x.j = 0; x.z = v; field_set(obj, (jfieldID)field_by_name(n), x); }
void jo_set_obj(jobject obj, const char *n, jobject v) { jvalue x; x.j = 0; x.l = v; field_set(obj, (jfieldID)field_by_name(n), x); }

void jo_set_string(jobject obj, const char *n, const char *utf8) {
    jstring s = utf8 ? (*jo_env())->NewStringUTF(jo_env(), utf8) : NULL;
    jo_set_obj(obj, n, s);
}

char *jo_jstring_to_utf8(jstring s) {
    if (!s) return NULL;
    JNIEnv *env = jo_env();
    const char *c = (*env)->GetStringUTFChars(env, s, NULL);
    if (!c) return NULL;
    char *r = strdup(c);
    (*env)->ReleaseStringUTFChars(env, s, (char *)c);
    return r;
}

JNIEnv *jo_env(void) {
    return &jni;
}

/*
 * Argument unpacking according to a method signature
 */

#define DJ_MAX_ARGS 16

static const char *sig_next(const char *p, char *type) {
    *type = *p;
    if (*p == 'L') {
        while (*p && *p != ';') p++;
        return *p ? p + 1 : p;
    }
    if (*p == '[') {
        while (*p == '[') p++;
        if (*p == 'L') {
            while (*p && *p != ';') p++;
            return *p ? p + 1 : p;
        }
        return *p ? p + 1 : p;
    }
    return p + 1;
}

static int args_from_va(const char *sig, va_list ap, jvalue *out) {
    const char *p = strchr(sig, '(');
    if (!p) return 0;
    p++;
    int n = 0;
    while (*p && *p != ')' && n < DJ_MAX_ARGS) {
        char t;
        p = sig_next(p, &t);
        jvalue v;
        v.j = 0;
        switch (t) {
            case 'J': v.j = va_arg(ap, jlong); break;
            case 'F': v.f = (jfloat)va_arg(ap, double); break;
            case 'D': v.d = va_arg(ap, double); break;
            case 'L':
            case '[': v.l = va_arg(ap, jobject); break;
            default:  v.i = va_arg(ap, jint); break;
        }
        out[n++] = v;
    }
    return n;
}

static int args_from_a(const char *sig, const jvalue *in, jvalue *out) {
    const char *p = strchr(sig, '(');
    if (!p) return 0;
    p++;
    int n = 0;
    while (*p && *p != ')' && n < DJ_MAX_ARGS) {
        char t;
        p = sig_next(p, &t);
        out[n] = in[n];
        n++;
    }
    return n;
}

/*
 * Method dispatch: this is the "Java" the game calls into (see jo_java_call).
 */

static jvalue dispatch(DJMethod *m, jobject self, const jvalue *a, int argc) {
    jvalue r;
    r.j = 0;

    if (!m) {
        l_error("[JO] Call on NULL method");
        return r;
    }
    return jo_java_call(m->cls ? m->cls->name : "", m->name, m->sig, self, a, argc);
}

static jvalue dispatch_static(DJMethod *m, const jvalue *a, int argc) {
    return dispatch(m, NULL, a, argc);
}

/*
 * JNIEnv replacements
 */

static jclass DJ_FindClass(JNIEnv *env, const char *name) {
    sceKernelLockLwMutex(&jo_lock, 1, NULL);
    DJClass *c = intern_class(name);
    sceKernelUnlockLwMutex(&jo_lock, 1);
    l_debug("[JO] FindClass(%s) = %p", name, c);
    return (jclass)c;
}

static jclass DJ_GetObjectClass(JNIEnv *env, jobject obj) {
    if (jo_is_object(obj)) return (jclass)((DJObject *)obj)->cls;
    return (jclass)intern_class("java/lang/Object");
}

static jboolean DJ_IsInstanceOf(JNIEnv *env, jobject obj, jclass clazz) {
    if (!obj) return JNI_TRUE;
    if (!jo_is_object(obj)) return JNI_FALSE;
    return ((DJObject *)obj)->cls == (DJClass *)clazz ? JNI_TRUE : JNI_FALSE;
}

static jobject DJ_AllocObject(JNIEnv *env, jclass clazz) {
    return new_object_of((DJClass *)clazz);
}

// Constructors run through jo_java_call(cls, "<init>", sig, obj, args)
static jobject construct(jclass clazz, DJMethod *m, const jvalue *a, int argc) {
    if (strcmp(((DJClass *)clazz)->name, "java/lang/String") == 0) {
        // new String(...) is a real (FalsoJNI) string, not a tracked object
        // (Unity makes one per frame: they filled the object table)
        JNIEnv *env = jo_env();
        if (m && argc >= 1 && strncmp(m->sig, "([B", 3) == 0 && a[0].l) {
            jsize n = (*env)->GetArrayLength(env, (jarray)a[0].l);
            jbyte *b = (*env)->GetByteArrayElements(env, (jbyteArray)a[0].l, NULL);
            char *s = malloc(n + 1);
            if (b) memcpy(s, b, n);
            s[b ? n : 0] = 0;
            jstring r = (*env)->NewStringUTF(env, s);
            free(s);
            return r;
        }
        if (m && argc >= 1 && strncmp(m->sig, "(Ljava/lang/String;", 19) == 0 && a[0].l) {
            char *s = jo_jstring_to_utf8((jstring)a[0].l);
            jstring r = (*env)->NewStringUTF(env, s ? s : "");
            free(s);
            return r;
        }
        return (*env)->NewStringUTF(env, "");
    }
    jobject o = new_object_of((DJClass *)clazz);
    if (m && strcmp(m->sig, "()V") != 0)
        jo_java_call(((DJClass *)clazz)->name, "<init>", m->sig, o, a, argc);
    return o;
}

static jobject DJ_NewObjectV(JNIEnv *env, jclass clazz, jmethodID mid, va_list ap) {
    DJMethod *m = (DJMethod *)mid;
    jvalue a[DJ_MAX_ARGS];
    int n = m ? args_from_va(m->sig, ap, a) : 0;
    return construct(clazz, m, a, n);
}

static jobject DJ_NewObject(JNIEnv *env, jclass clazz, jmethodID mid, ...) {
    va_list ap;
    va_start(ap, mid);
    jobject r = DJ_NewObjectV(env, clazz, mid, ap);
    va_end(ap);
    return r;
}

static jobject DJ_NewObjectA(JNIEnv *env, jclass clazz, jmethodID mid, const jvalue *args) {
    DJMethod *m = (DJMethod *)mid;
    jvalue a[DJ_MAX_ARGS];
    int n = m ? args_from_a(m->sig, args, a) : 0;
    return construct(clazz, m, a, n);
}

static jmethodID DJ_GetMethodID(JNIEnv *env, jclass clazz, const char *name, const char *sig) {
    sceKernelLockLwMutex(&jo_lock, 1, NULL);
    DJMethod *m = intern_method((DJClass *)clazz, name, sig);
    sceKernelUnlockLwMutex(&jo_lock, 1);
    l_debug("[JO] GetMethodID(%s, %s%s)", ((DJClass *)clazz)->name, name, sig);
    return (jmethodID)m;
}

// java.lang.reflect.Method objects (made by na_looper.c) carry their jmethodID
static jmethodID DJ_FromReflectedMethod(JNIEnv *env, jobject method) {
    if (jo_is_object(method)) return (jmethodID)(uintptr_t)jo_get_long(method, "__mid");
    return NULL;
}

// java.lang.reflect.Field objects (ReflectionHelper.getFieldID, na_java.c) carry their jfieldID
static jfieldID DJ_FromReflectedField(JNIEnv *env, jobject field) {
    if (jo_is_object(field)) return (jfieldID)(uintptr_t)jo_get_long(field, "__fid");
    return NULL;
}

static jobject DJ_ToReflectedMethod(JNIEnv *env, jclass cls, jmethodID mid, jboolean is_static) {
    DJMethod *m = (DJMethod *)mid;
    jobject o = jo_new_object("java/lang/reflect/Method");
    jo_set_long(o, "__mid", (jlong)(uintptr_t)mid);
    if (m) jo_set_string(o, "__name", m->name);
    jo_set_obj(o, "__class", (jobject)cls);
    return o;
}

static jfieldID DJ_GetFieldID(JNIEnv *env, jclass clazz, const char *name, const char *sig) {
    sceKernelLockLwMutex(&jo_lock, 1, NULL);
    DJField *f = intern_field(name, sig);
    sceKernelUnlockLwMutex(&jo_lock, 1);
    return (jfieldID)f;
}

static void DJ_DeleteLocalRef(JNIEnv *env, jobject obj) {
    // Message objects are short-lived and only referenced by the game.
    // Everything else (strings/arrays) is left to FalsoJNI semantics: ignored.
}

#define DJ_GET_FIELD(Name, Type, member) \
    static Type DJ_Get##Name##Field(JNIEnv *env, jobject obj, jfieldID fid) { \
        return field_get(obj, fid).member; \
    }

#define DJ_SET_FIELD(Name, Type, member) \
    static void DJ_Set##Name##Field(JNIEnv *env, jobject obj, jfieldID fid, Type val) { \
        jvalue v; v.j = 0; v.member = val; field_set(obj, fid, v); \
    }

DJ_GET_FIELD(Object, jobject, l)
DJ_GET_FIELD(Boolean, jboolean, z)
DJ_GET_FIELD(Byte, jbyte, b)
DJ_GET_FIELD(Char, jchar, c)
DJ_GET_FIELD(Short, jshort, s)
DJ_GET_FIELD(Int, jint, i)
DJ_GET_FIELD(Long, jlong, j)
DJ_GET_FIELD(Float, jfloat, f)
DJ_GET_FIELD(Double, jdouble, d)

DJ_SET_FIELD(Object, jobject, l)
DJ_SET_FIELD(Boolean, jboolean, z)
DJ_SET_FIELD(Byte, jbyte, b)
DJ_SET_FIELD(Char, jchar, c)
DJ_SET_FIELD(Short, jshort, s)
DJ_SET_FIELD(Int, jint, i)
DJ_SET_FIELD(Long, jlong, j)
DJ_SET_FIELD(Float, jfloat, f)
DJ_SET_FIELD(Double, jdouble, d)

static jmethodID DJ_GetStaticMethodID(JNIEnv *env, jclass clazz, const char *name, const char *sig) {
    return DJ_GetMethodID(env, clazz, name, sig);
}

#define DJ_CALL_STATIC(Name, Type, member) \
    static Type DJ_CallStatic##Name##MethodV(JNIEnv *env, jclass clazz, jmethodID mid, va_list ap) { \
        jvalue a[DJ_MAX_ARGS]; \
        int n = args_from_va(((DJMethod *)mid)->sig, ap, a); \
        return (Type)dispatch_static((DJMethod *)mid, a, n).member; \
    } \
    static Type DJ_CallStatic##Name##Method(JNIEnv *env, jclass clazz, jmethodID mid, ...) { \
        va_list ap; \
        va_start(ap, mid); \
        Type r = DJ_CallStatic##Name##MethodV(env, clazz, mid, ap); \
        va_end(ap); \
        return r; \
    } \
    static Type DJ_CallStatic##Name##MethodA(JNIEnv *env, jclass clazz, jmethodID mid, const jvalue *in) { \
        jvalue a[DJ_MAX_ARGS]; \
        int n = args_from_a(((DJMethod *)mid)->sig, in, a); \
        return (Type)dispatch_static((DJMethod *)mid, a, n).member; \
    }

DJ_CALL_STATIC(Object, jobject, l)
DJ_CALL_STATIC(Boolean, jboolean, z)
DJ_CALL_STATIC(Int, jint, i)
DJ_CALL_STATIC(Long, jlong, j)
DJ_CALL_STATIC(Float, jfloat, f)
DJ_CALL_STATIC(Double, jdouble, d)
DJ_CALL_STATIC(Short, jshort, s)
DJ_CALL_STATIC(Byte, jbyte, b)
DJ_CALL_STATIC(Char, jchar, c)

static void DJ_CallStaticVoidMethodV(JNIEnv *env, jclass clazz, jmethodID mid, va_list ap) {
    jvalue a[DJ_MAX_ARGS];
    int n = args_from_va(((DJMethod *)mid)->sig, ap, a);
    dispatch_static((DJMethod *)mid, a, n);
}

static void DJ_CallStaticVoidMethod(JNIEnv *env, jclass clazz, jmethodID mid, ...) {
    va_list ap;
    va_start(ap, mid);
    DJ_CallStaticVoidMethodV(env, clazz, mid, ap);
    va_end(ap);
}

static void DJ_CallStaticVoidMethodA(JNIEnv *env, jclass clazz, jmethodID mid, const jvalue *in) {
    jvalue a[DJ_MAX_ARGS];
    int n = args_from_a(((DJMethod *)mid)->sig, in, a);
    dispatch_static((DJMethod *)mid, a, n);
}

#define DJ_CALL_INSTANCE(Name, Type, member) \
    static Type DJ_Call##Name##MethodV(JNIEnv *env, jobject obj, jmethodID mid, va_list ap) { \
        jvalue a[DJ_MAX_ARGS]; \
        int n = args_from_va(((DJMethod *)mid)->sig, ap, a); \
        return (Type)dispatch((DJMethod *)mid, obj, a, n).member; \
    } \
    static Type DJ_Call##Name##Method(JNIEnv *env, jobject obj, jmethodID mid, ...) { \
        va_list ap; \
        va_start(ap, mid); \
        Type r = DJ_Call##Name##MethodV(env, obj, mid, ap); \
        va_end(ap); \
        return r; \
    } \
    static Type DJ_Call##Name##MethodA(JNIEnv *env, jobject obj, jmethodID mid, const jvalue *in) { \
        jvalue a[DJ_MAX_ARGS]; \
        int n = args_from_a(((DJMethod *)mid)->sig, in, a); \
        return (Type)dispatch((DJMethod *)mid, obj, a, n).member; \
    }

DJ_CALL_INSTANCE(Object, jobject, l)
DJ_CALL_INSTANCE(Boolean, jboolean, z)
DJ_CALL_INSTANCE(Int, jint, i)
DJ_CALL_INSTANCE(Long, jlong, j)
DJ_CALL_INSTANCE(Float, jfloat, f)
DJ_CALL_INSTANCE(Double, jdouble, d)
DJ_CALL_INSTANCE(Short, jshort, s)
DJ_CALL_INSTANCE(Byte, jbyte, b)
DJ_CALL_INSTANCE(Char, jchar, c)

static void DJ_CallVoidMethodV(JNIEnv *env, jobject obj, jmethodID mid, va_list ap) {
    jvalue a[DJ_MAX_ARGS];
    int n = args_from_va(((DJMethod *)mid)->sig, ap, a);
    dispatch((DJMethod *)mid, obj, a, n);
}

static void DJ_CallVoidMethod(JNIEnv *env, jobject obj, jmethodID mid, ...) {
    va_list ap;
    va_start(ap, mid);
    DJ_CallVoidMethodV(env, obj, mid, ap);
    va_end(ap);
}

static void DJ_CallVoidMethodA(JNIEnv *env, jobject obj, jmethodID mid, const jvalue *in) {
    jvalue a[DJ_MAX_ARGS];
    int n = args_from_a(((DJMethod *)mid)->sig, in, a);
    dispatch((DJMethod *)mid, obj, a, n);
}

// Static fields: the port answers them (jo_java_static_field)
static jfieldID DJ_GetStaticFieldID(JNIEnv *env, jclass clazz, const char *name, const char *sig) {
    return DJ_GetFieldID(env, clazz, name, sig);
}

static jvalue static_field(jclass clazz, jfieldID fid) {
    DJField *f = (DJField *)fid;
    return jo_java_static_field(clazz ? ((DJClass *)clazz)->name : "", f ? f->name : "", f ? f->sig : "");
}

#define DJ_GET_STATIC_FIELD(Name, Type, member) \
    static Type DJ_GetStatic##Name##Field(JNIEnv *env, jclass clazz, jfieldID fid) { \
        return (Type)static_field(clazz, fid).member; \
    }

DJ_GET_STATIC_FIELD(Object, jobject, l)
DJ_GET_STATIC_FIELD(Boolean, jboolean, z)
DJ_GET_STATIC_FIELD(Int, jint, i)
DJ_GET_STATIC_FIELD(Long, jlong, j)
DJ_GET_STATIC_FIELD(Float, jfloat, f)
DJ_GET_STATIC_FIELD(Double, jdouble, d)
DJ_GET_STATIC_FIELD(Short, jshort, s)
DJ_GET_STATIC_FIELD(Byte, jbyte, b)
DJ_GET_STATIC_FIELD(Char, jchar, c)

// Non-virtual calls behave like virtual ones here
#define DJ_CALL_NONVIRT(Name, Type) \
    static Type DJ_CallNonvirtual##Name##MethodV(JNIEnv *env, jobject obj, jclass c, jmethodID mid, va_list ap) { \
        return DJ_Call##Name##MethodV(env, obj, mid, ap); \
    } \
    static Type DJ_CallNonvirtual##Name##Method(JNIEnv *env, jobject obj, jclass c, jmethodID mid, ...) { \
        va_list ap; \
        va_start(ap, mid); \
        Type r = DJ_Call##Name##MethodV(env, obj, mid, ap); \
        va_end(ap); \
        return r; \
    } \
    static Type DJ_CallNonvirtual##Name##MethodA(JNIEnv *env, jobject obj, jclass c, jmethodID mid, const jvalue *a) { \
        return DJ_Call##Name##MethodA(env, obj, mid, a); \
    }

DJ_CALL_NONVIRT(Object, jobject)
DJ_CALL_NONVIRT(Boolean, jboolean)
DJ_CALL_NONVIRT(Int, jint)
DJ_CALL_NONVIRT(Long, jlong)
DJ_CALL_NONVIRT(Float, jfloat)

static void DJ_CallNonvirtualVoidMethodV(JNIEnv *env, jobject obj, jclass c, jmethodID mid, va_list ap) {
    DJ_CallVoidMethodV(env, obj, mid, ap);
}
static void DJ_CallNonvirtualVoidMethod(JNIEnv *env, jobject obj, jclass c, jmethodID mid, ...) {
    va_list ap;
    va_start(ap, mid);
    DJ_CallVoidMethodV(env, obj, mid, ap);
    va_end(ap);
}
static void DJ_CallNonvirtualVoidMethodA(JNIEnv *env, jobject obj, jclass c, jmethodID mid, const jvalue *a) {
    DJ_CallVoidMethodA(env, obj, mid, a);
}

// RegisterNatives: handed to the port, which calls the natives later
static jint DJ_RegisterNatives(JNIEnv *env, jclass clazz, const JNINativeMethod *methods, jint n) {
    jo_register_natives(clazz ? ((DJClass *)clazz)->name : "", methods, n);
    return JNI_OK;
}

static jint DJ_UnregisterNatives(JNIEnv *env, jclass clazz) {
    return JNI_OK;
}

static jclass DJ_GetSuperclass(JNIEnv *env, jclass clazz) {
    return (jclass)intern_class("java/lang/Object");
}

static jboolean DJ_IsAssignableFrom(JNIEnv *env, jclass a, jclass b) {
    return JNI_TRUE;
}

static jboolean DJ_IsSameObject(JNIEnv *env, jobject a, jobject b) {
    return a == b ? JNI_TRUE : JNI_FALSE;
}

/*
 * Direct byte buffers (Ljava/nio/Buffer;): a DJObject holding a pointer
 */

static jobject DJ_NewDirectByteBuffer(JNIEnv *env, void *address, jlong capacity) {
    jobject o = jo_new_object("java/nio/ByteBuffer");
    jo_set_long(o, "__address", (jlong)(uintptr_t)address);
    jo_set_long(o, "__capacity", capacity);
    return o;
}

static void *DJ_GetDirectBufferAddress(JNIEnv *env, jobject buf) {
    return (void *)(uintptr_t)jo_get_long(buf, "__address");
}

static jlong DJ_GetDirectBufferCapacity(JNIEnv *env, jobject buf) {
    return jo_get_long(buf, "__capacity");
}

/*
 * Strings and arrays stay FalsoJNI's, but the native code sometimes hands
 * them one of our objects or classes (a default answer where it expected a
 * String, a Class it treats as an object). FalsoJNI would read that as a
 * JavaString and copy a garbage length into the heap. These wrappers check
 * the pointer first; they also fix GetStringUTFLength (UTF-8 length, not
 * UTF-16) and GetStringUTFRegion (copied from the wrong address).
 */
#include <falso_jni/FalsoJNI_ImplBridge.h>

#define JDA_MAGIC_ 0x4A444121

static struct JNINativeInterface orig;

static int is_class(const void *p) {
    int r = 0;
    sceKernelLockLwMutex(&jo_lock, 1, NULL);
    for (DJClass *c = classes; c; c = c->next)
        if ((const void *)c == p) { r = 1; break; }
    sceKernelUnlockLwMutex(&jo_lock, 1);
    return r;
}

static int not_falso(const void *p, const char *what) {
    if (!p) return 1;
    if (jo_is_object((jobject)p) || is_class(p)) {
        static int warned = 0;
        if (warned++ < 16)
            l_warn("[JO] %s on %s %p (%s)", what, jo_is_object((jobject)p) ? "object" : "class", p,
                   jo_is_object((jobject)p) ? jo_class_name((jobject)p) : ((DJClass *)p)->name);
        return 1;
    }
    return 0;
}

static const char *DJ_GetStringUTFChars(JNIEnv *env, jstring s, jboolean *isCopy) {
    if (isCopy) *isCopy = JNI_TRUE;
    if (not_falso(s, "GetStringUTFChars")) return strdup("");
    return orig.GetStringUTFChars(env, s, isCopy);
}

static jsize DJ_GetStringUTFLength(JNIEnv *env, jstring s) {
    if (not_falso(s, "GetStringUTFLength")) return 0;
    const char *c = orig.GetStringUTFChars(env, s, NULL);
    jsize n = c ? (jsize)strlen(c) : 0;
    free((void *)c);
    return n;
}

static jsize DJ_GetStringLength(JNIEnv *env, jstring s) {
    if (not_falso(s, "GetStringLength")) return 0;
    return orig.GetStringLength(env, s);
}

static const jchar *DJ_GetStringChars(JNIEnv *env, jstring s, jboolean *isCopy) {
    if (not_falso(s, "GetStringChars")) return calloc(1, sizeof(jchar));
    return orig.GetStringChars(env, s, isCopy);
}

// start/len count UTF-16 units; the strings here are ASCII, so bytes == units
static void DJ_GetStringUTFRegion(JNIEnv *env, jstring s, jsize start, jsize len, char *buf) {
    if (!buf) return;
    buf[0] = 0;
    if (not_falso(s, "GetStringUTFRegion")) return;
    const char *c = orig.GetStringUTFChars(env, s, NULL);
    if (!c) return;
    jsize n = (jsize)strlen(c);
    if (start < 0) start = 0;
    if (start > n) start = n;
    if (len > n - start) len = n - start;
    if (len < 0) len = 0;
    memcpy(buf, c + start, len);
    buf[len] = 0;
    free((void *)c);
}

static void DJ_GetStringRegion(JNIEnv *env, jstring s, jsize start, jsize len, jchar *buf) {
    if (not_falso(s, "GetStringRegion")) return;
    orig.GetStringRegion(env, s, start, len, buf);
}

static jsize DJ_GetArrayLength(JNIEnv *env, jarray a) {
    if (not_falso(a, "GetArrayLength")) return 0;
    return orig.GetArrayLength(env, a);
}

static jobject DJ_GetObjectArrayElement(JNIEnv *env, jobjectArray a, jsize i) {
    if (not_falso(a, "GetObjectArrayElement")) return NULL;
    return orig.GetObjectArrayElement(env, a, i);
}

static jbyte *DJ_GetByteArrayElements(JNIEnv *env, jbyteArray a, jboolean *isCopy) {
    if (not_falso(a, "GetByteArrayElements")) return NULL;
    return orig.GetByteArrayElements(env, a, isCopy);
}

static void DJ_GetByteArrayRegion(JNIEnv *env, jbyteArray a, jsize start, jsize len, jbyte *buf) {
    if (not_falso(a, "GetByteArrayRegion")) return;
    orig.GetByteArrayRegion(env, a, start, len, buf);
}

// FalsoJNI's DeleteGlobalRef free()s the object. Unity drops global refs to
// classes and objects that stay in use (our class list, interned strings),
// so references are never freed here; the leak is small.
static void DJ_DeleteGlobalRef(JNIEnv *env, jobject obj) {}

void jni_objects_install(void) {
    sceKernelCreateLwMutex(&jo_lock, "jni_objects", SCE_KERNEL_MUTEX_ATTR_RECURSIVE, 0, NULL);

    struct JNINativeInterface *t = (struct JNINativeInterface *)jni;
    orig = *t;

    t->GetStringUTFChars = DJ_GetStringUTFChars;
    t->GetStringUTFLength = DJ_GetStringUTFLength;
    t->GetStringLength = DJ_GetStringLength;
    t->GetStringChars = DJ_GetStringChars;
    t->GetStringUTFRegion = DJ_GetStringUTFRegion;
    t->GetStringRegion = DJ_GetStringRegion;
    t->GetArrayLength = DJ_GetArrayLength;
    t->GetObjectArrayElement = DJ_GetObjectArrayElement;
    t->GetByteArrayElements = DJ_GetByteArrayElements;
    t->GetByteArrayRegion = DJ_GetByteArrayRegion;
    t->DeleteGlobalRef = DJ_DeleteGlobalRef;
    t->DeleteWeakGlobalRef = DJ_DeleteGlobalRef;

    t->FindClass = DJ_FindClass;
    t->GetObjectClass = DJ_GetObjectClass;
    t->IsInstanceOf = DJ_IsInstanceOf;
    t->AllocObject = DJ_AllocObject;
    t->NewObject = DJ_NewObject;
    t->NewObjectV = DJ_NewObjectV;
    t->NewObjectA = DJ_NewObjectA;
    t->DeleteLocalRef = DJ_DeleteLocalRef;

    t->GetMethodID = DJ_GetMethodID;
    t->FromReflectedMethod = DJ_FromReflectedMethod;
    t->FromReflectedField = DJ_FromReflectedField;
    t->ToReflectedMethod = DJ_ToReflectedMethod;
    t->GetFieldID = DJ_GetFieldID;

    t->GetObjectField = DJ_GetObjectField;
    t->GetBooleanField = DJ_GetBooleanField;
    t->GetByteField = DJ_GetByteField;
    t->GetCharField = DJ_GetCharField;
    t->GetShortField = DJ_GetShortField;
    t->GetIntField = DJ_GetIntField;
    t->GetLongField = DJ_GetLongField;
    t->GetFloatField = DJ_GetFloatField;
    t->GetDoubleField = DJ_GetDoubleField;

    t->SetObjectField = DJ_SetObjectField;
    t->SetBooleanField = DJ_SetBooleanField;
    t->SetByteField = DJ_SetByteField;
    t->SetCharField = DJ_SetCharField;
    t->SetShortField = DJ_SetShortField;
    t->SetIntField = DJ_SetIntField;
    t->SetLongField = DJ_SetLongField;
    t->SetFloatField = DJ_SetFloatField;
    t->SetDoubleField = DJ_SetDoubleField;

    t->CallObjectMethod = DJ_CallObjectMethod;
    t->CallObjectMethodV = DJ_CallObjectMethodV;
    t->CallObjectMethodA = DJ_CallObjectMethodA;
    t->CallBooleanMethod = DJ_CallBooleanMethod;
    t->CallBooleanMethodV = DJ_CallBooleanMethodV;
    t->CallBooleanMethodA = DJ_CallBooleanMethodA;
    t->CallIntMethod = DJ_CallIntMethod;
    t->CallIntMethodV = DJ_CallIntMethodV;
    t->CallIntMethodA = DJ_CallIntMethodA;
    t->CallLongMethod = DJ_CallLongMethod;
    t->CallLongMethodV = DJ_CallLongMethodV;
    t->CallLongMethodA = DJ_CallLongMethodA;
    t->CallFloatMethod = DJ_CallFloatMethod;
    t->CallFloatMethodV = DJ_CallFloatMethodV;
    t->CallFloatMethodA = DJ_CallFloatMethodA;
    t->CallVoidMethod = DJ_CallVoidMethod;
    t->CallVoidMethodV = DJ_CallVoidMethodV;
    t->CallVoidMethodA = DJ_CallVoidMethodA;
    t->CallDoubleMethod = DJ_CallDoubleMethod;
    t->CallDoubleMethodV = DJ_CallDoubleMethodV;
    t->CallDoubleMethodA = DJ_CallDoubleMethodA;
    t->CallShortMethod = DJ_CallShortMethod;
    t->CallShortMethodV = DJ_CallShortMethodV;
    t->CallShortMethodA = DJ_CallShortMethodA;
    t->CallByteMethod = DJ_CallByteMethod;
    t->CallByteMethodV = DJ_CallByteMethodV;
    t->CallByteMethodA = DJ_CallByteMethodA;
    t->CallCharMethod = DJ_CallCharMethod;
    t->CallCharMethodV = DJ_CallCharMethodV;
    t->CallCharMethodA = DJ_CallCharMethodA;

    t->CallNonvirtualObjectMethod = DJ_CallNonvirtualObjectMethod;
    t->CallNonvirtualObjectMethodV = DJ_CallNonvirtualObjectMethodV;
    t->CallNonvirtualObjectMethodA = DJ_CallNonvirtualObjectMethodA;
    t->CallNonvirtualBooleanMethod = DJ_CallNonvirtualBooleanMethod;
    t->CallNonvirtualBooleanMethodV = DJ_CallNonvirtualBooleanMethodV;
    t->CallNonvirtualBooleanMethodA = DJ_CallNonvirtualBooleanMethodA;
    t->CallNonvirtualIntMethod = DJ_CallNonvirtualIntMethod;
    t->CallNonvirtualIntMethodV = DJ_CallNonvirtualIntMethodV;
    t->CallNonvirtualIntMethodA = DJ_CallNonvirtualIntMethodA;
    t->CallNonvirtualLongMethod = DJ_CallNonvirtualLongMethod;
    t->CallNonvirtualLongMethodV = DJ_CallNonvirtualLongMethodV;
    t->CallNonvirtualLongMethodA = DJ_CallNonvirtualLongMethodA;
    t->CallNonvirtualFloatMethod = DJ_CallNonvirtualFloatMethod;
    t->CallNonvirtualFloatMethodV = DJ_CallNonvirtualFloatMethodV;
    t->CallNonvirtualFloatMethodA = DJ_CallNonvirtualFloatMethodA;
    t->CallNonvirtualVoidMethod = DJ_CallNonvirtualVoidMethod;
    t->CallNonvirtualVoidMethodV = DJ_CallNonvirtualVoidMethodV;
    t->CallNonvirtualVoidMethodA = DJ_CallNonvirtualVoidMethodA;

    t->RegisterNatives = DJ_RegisterNatives;
    t->UnregisterNatives = DJ_UnregisterNatives;
    t->GetSuperclass = DJ_GetSuperclass;
    t->IsAssignableFrom = DJ_IsAssignableFrom;
    t->IsSameObject = DJ_IsSameObject;

    t->GetStaticMethodID = DJ_GetStaticMethodID;
    t->CallStaticObjectMethod = DJ_CallStaticObjectMethod;
    t->CallStaticObjectMethodV = DJ_CallStaticObjectMethodV;
    t->CallStaticObjectMethodA = DJ_CallStaticObjectMethodA;
    t->CallStaticBooleanMethod = DJ_CallStaticBooleanMethod;
    t->CallStaticBooleanMethodV = DJ_CallStaticBooleanMethodV;
    t->CallStaticBooleanMethodA = DJ_CallStaticBooleanMethodA;
    t->CallStaticIntMethod = DJ_CallStaticIntMethod;
    t->CallStaticIntMethodV = DJ_CallStaticIntMethodV;
    t->CallStaticIntMethodA = DJ_CallStaticIntMethodA;
    t->CallStaticLongMethod = DJ_CallStaticLongMethod;
    t->CallStaticLongMethodV = DJ_CallStaticLongMethodV;
    t->CallStaticLongMethodA = DJ_CallStaticLongMethodA;
    t->CallStaticFloatMethod = DJ_CallStaticFloatMethod;
    t->CallStaticFloatMethodV = DJ_CallStaticFloatMethodV;
    t->CallStaticFloatMethodA = DJ_CallStaticFloatMethodA;
    t->CallStaticVoidMethod = DJ_CallStaticVoidMethod;
    t->CallStaticVoidMethodV = DJ_CallStaticVoidMethodV;
    t->CallStaticVoidMethodA = DJ_CallStaticVoidMethodA;
    t->CallStaticDoubleMethod = DJ_CallStaticDoubleMethod;
    t->CallStaticDoubleMethodV = DJ_CallStaticDoubleMethodV;
    t->CallStaticDoubleMethodA = DJ_CallStaticDoubleMethodA;
    t->CallStaticShortMethod = DJ_CallStaticShortMethod;
    t->CallStaticShortMethodV = DJ_CallStaticShortMethodV;
    t->CallStaticShortMethodA = DJ_CallStaticShortMethodA;
    t->CallStaticByteMethod = DJ_CallStaticByteMethod;
    t->CallStaticByteMethodV = DJ_CallStaticByteMethodV;
    t->CallStaticByteMethodA = DJ_CallStaticByteMethodA;
    t->CallStaticCharMethod = DJ_CallStaticCharMethod;
    t->CallStaticCharMethodV = DJ_CallStaticCharMethodV;
    t->CallStaticCharMethodA = DJ_CallStaticCharMethodA;

    t->GetStaticFieldID = DJ_GetStaticFieldID;
    t->GetStaticObjectField = DJ_GetStaticObjectField;
    t->GetStaticBooleanField = DJ_GetStaticBooleanField;
    t->GetStaticIntField = DJ_GetStaticIntField;
    t->GetStaticLongField = DJ_GetStaticLongField;
    t->GetStaticFloatField = DJ_GetStaticFloatField;
    t->GetStaticDoubleField = DJ_GetStaticDoubleField;
    t->GetStaticShortField = DJ_GetStaticShortField;
    t->GetStaticByteField = DJ_GetStaticByteField;
    t->GetStaticCharField = DJ_GetStaticCharField;

    t->NewDirectByteBuffer = DJ_NewDirectByteBuffer;
    t->GetDirectBufferAddress = DJ_GetDirectBufferAddress;
    t->GetDirectBufferCapacity = DJ_GetDirectBufferCapacity;

    l_success("[JO] JNI object model installed.");
}
