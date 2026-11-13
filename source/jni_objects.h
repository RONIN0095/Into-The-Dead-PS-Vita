/*
 * Copyright (C) 2026 Vita port contributors
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

/**
 * @file  jni_objects.h
 * @brief Minimal per-object Java emulation on top of FalsoJNI.
 *
 * Games pass objects to their Java side and read their fields back, often
 * per instance. FalsoJNI only knows about global fields, so the object, field
 * and method-call parts of its JNIEnv are replaced here.
 */

#ifndef JNI_OBJECTS_H
#define JNI_OBJECTS_H

#include <falso_jni/FalsoJNI.h>

void jni_objects_install(void);

JNIEnv *jo_env(void);

jobject jo_new_object(const char *class_name);
void jo_free_object(jobject obj);
/** A copy of one of our objects (same class, same field values). */
jobject jo_clone_object(jobject obj);
int jo_is_object(jobject obj);
const char *jo_class_name(jobject obj);

jint jo_get_int(jobject obj, const char *field);
jlong jo_get_long(jobject obj, const char *field);
jfloat jo_get_float(jobject obj, const char *field);
jboolean jo_get_bool(jobject obj, const char *field);
jobject jo_get_obj(jobject obj, const char *field);
// Returns a malloc'd UTF-8 copy of a String field, or NULL.
char *jo_get_string(jobject obj, const char *field);

void jo_set_int(jobject obj, const char *field, jint v);
void jo_set_long(jobject obj, const char *field, jlong v);
void jo_set_float(jobject obj, const char *field, jfloat v);
void jo_set_bool(jobject obj, const char *field, jboolean v);
void jo_set_obj(jobject obj, const char *field, jobject v);
void jo_set_string(jobject obj, const char *field, const char *utf8);

// Returns a malloc'd UTF-8 copy of a Java string, or NULL.
char *jo_jstring_to_utf8(jstring s);

// Implemented by the port: every Java method the game calls ends up here.
// self is NULL for static methods. Unused return values are ignored.
jvalue jo_java_call(const char *cls, const char *name, const char *sig,
                    jobject self, const jvalue *args, int argc);

// Implemented by the port: value of a static Java field (Build.MODEL, ...).
jvalue jo_java_static_field(const char *cls, const char *name, const char *sig);

// Implemented by the port: natives the library registers (RegisterNatives).
void jo_register_natives(const char *cls, const JNINativeMethod *methods, int n);

#endif // JNI_OBJECTS_H
