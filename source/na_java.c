/*
 * Copyright (C) 2026 Vita port contributors
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

/**
 * @file  na_java.c
 * @brief Stand-ins for the Java side of a Unity 6 player.
 *
 * Every Java method libunity.so calls ends up in jo_java_call() (see
 * jni_objects.c). Known methods are answered here: the Context (package
 * name, APK path, directories), Build, Locale, DisplayMetrics,
 * SharedPreferences (PlayerPrefs) and a few services. Everything else is
 * logged once and answered with a neutral value: 0/false, "" for strings,
 * and a fresh object of the declared return class for objects, so that
 * call chains like getResources().getConfiguration().locale do not crash.
 */

#include "jni_objects.h"
#include "na_looper.h"

#include <ctype.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <strings.h>
#include <sys/stat.h>

#include <psp2/kernel/processmgr.h>

#include "na_android.h"
#include "na_apk.h"
#include "na_input.h"
#include "utils/logger.h"

#define APK_PATH DATA_PATH "base.apk"

static JNIEnv *env(void) { return jo_env(); }

static jstring jstr(const char *s) {
    return (*env())->NewStringUTF(env(), s);
}

static int is(const char *a, const char *b) {
    return strcmp(a, b) == 0;
}

static int ends_with(const char *s, const char *suffix) {
    size_t a = strlen(s), b = strlen(suffix);
    return a >= b && strcmp(s + a - b, suffix) == 0;
}

static jobject file_object(const char *path) {
    jobject f = jo_new_object("java/io/File");
    jo_set_string(f, "path", path);
    return f;
}

/*
 * SharedPreferences (PlayerPrefs): one key=value text file per preferences name
 */
#define MAX_PREFS 512

typedef struct {
    char key[128];
    char type; // 's', 'i', 'l', 'f', 'b'
    char *value;
} pref;

static pref prefs[MAX_PREFS];
static int n_prefs = 0;
static int prefs_loaded = 0;

#define PREFS_FILE DATA_PATH "prefs.txt"

static void prefs_load(void) {
    if (prefs_loaded) return;
    prefs_loaded = 1;
    FILE *f = fopen(PREFS_FILE, "r");
    if (!f) return;
    char line[4096];
    while (fgets(line, sizeof(line), f) && n_prefs < MAX_PREFS) {
        line[strcspn(line, "\r\n")] = 0;
        // format: <type> <key>\t<value>
        if (strlen(line) < 3 || line[1] != ' ') continue;
        char *tab = strchr(line + 2, '\t');
        if (!tab) continue;
        *tab = 0;
        pref *p = &prefs[n_prefs++];
        p->type = line[0];
        strncpy(p->key, line + 2, sizeof(p->key) - 1);
        p->value = strdup(tab + 1);
    }
    fclose(f);
    l_info("[prefs] %d values loaded", n_prefs);
}

/*
 * Saving: the file is written by a thread of its own. The game saves at each
 * checkpoint (and for coins, kills, ...), and writing the file on the memory
 * card held the main thread for 0.2-0.4 s: the short freeze there. The text
 * is put together here and handed over; the thread writes the newest one.
 */
static pthread_mutex_t save_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t save_cond = PTHREAD_COND_INITIALIZER;
static char *save_text = NULL; // waiting to be written
static int save_thread_up = 0;

static void *prefs_writer(void *arg) {
    (void)arg;
    for (;;) {
        pthread_mutex_lock(&save_lock);
        while (!save_text) pthread_cond_wait(&save_cond, &save_lock);
        char *text = save_text;
        save_text = NULL;
        pthread_mutex_unlock(&save_lock);
        FILE *f = fopen(PREFS_FILE ".tmp", "w");
        if (f) {
            size_t n = strlen(text);
            int ok = fwrite(text, 1, n, f) == n;
            ok = fclose(f) == 0 && ok;
            if (ok) {
                remove(PREFS_FILE);
                rename(PREFS_FILE ".tmp", PREFS_FILE);
            }
        }
        free(text);
    }
    return NULL;
}

static void prefs_save(void) {
    size_t cap = 256;
    for (int i = 0; i < n_prefs; i++)
        if (prefs[i].value) cap += strlen(prefs[i].key) + strlen(prefs[i].value) + 8;
    char *text = malloc(cap);
    if (!text) return;
    size_t len = 0;
    for (int i = 0; i < n_prefs; i++)
        if (prefs[i].value)
            len += snprintf(text + len, cap - len, "%c %s\t%s\n", prefs[i].type, prefs[i].key, prefs[i].value);
    text[len] = 0;
    pthread_mutex_lock(&save_lock);
    if (!save_thread_up) {
        pthread_t th;
        pthread_attr_t a;
        pthread_attr_init(&a);
        pthread_attr_setstacksize(&a, 64 * 1024);
        save_thread_up = pthread_create(&th, &a, prefs_writer, NULL) == 0;
        pthread_attr_destroy(&a);
    }
    if (save_thread_up) {
        free(save_text); // an older one not written yet
        save_text = text;
        text = NULL;
        pthread_cond_signal(&save_cond);
    }
    pthread_mutex_unlock(&save_lock);
    if (text) { // no thread: write it here
        FILE *f = fopen(PREFS_FILE ".tmp", "w");
        if (f) {
            fwrite(text, 1, len, f);
            fclose(f);
            remove(PREFS_FILE);
            rename(PREFS_FILE ".tmp", PREFS_FILE);
        }
        free(text);
    }
}

static pref *pref_find(const char *key) {
    prefs_load();
    for (int i = 0; i < n_prefs; i++)
        if (prefs[i].value && strcmp(prefs[i].key, key) == 0) return &prefs[i];
    return NULL;
}

static void pref_set(const char *key, char type, const char *value) {
    pref *p = pref_find(key);
    if (!p) {
        for (int i = 0; i < n_prefs; i++)
            if (!prefs[i].value) { p = &prefs[i]; break; }
        if (!p && n_prefs < MAX_PREFS) p = &prefs[n_prefs++];
        if (!p) return;
        strncpy(p->key, key, sizeof(p->key) - 1);
    } else {
        free(p->value);
    }
    p->type = type;
    p->value = strdup(value);
}

static void pref_remove(const char *key) {
    pref *p = pref_find(key);
    if (p) {
        free(p->value);
        p->value = NULL;
    }
}

static jvalue prefs_call(const char *name, const jvalue *args, int argc) {
    jvalue r;
    r.j = 0;
    char *key = argc >= 1 ? jo_jstring_to_utf8((jstring)args[0].l) : NULL;
    char buf[64];

    if (is(name, "getString")) {
        pref *p = key ? pref_find(key) : NULL;
        r.l = p ? jstr(p->value) : (argc >= 2 ? args[1].l : NULL);
    } else if (is(name, "getInt")) {
        pref *p = key ? pref_find(key) : NULL;
        r.i = p ? atoi(p->value) : args[1].i;
    } else if (is(name, "getLong")) {
        pref *p = key ? pref_find(key) : NULL;
        r.j = p ? atoll(p->value) : args[1].j;
    } else if (is(name, "getFloat")) {
        pref *p = key ? pref_find(key) : NULL;
        r.f = p ? (float)atof(p->value) : args[1].f;
    } else if (is(name, "getBoolean")) {
        pref *p = key ? pref_find(key) : NULL;
        r.z = p ? (jboolean)atoi(p->value) : args[1].z;
    } else if (is(name, "contains")) {
        r.z = key && pref_find(key) ? JNI_TRUE : JNI_FALSE;
    } else if (is(name, "edit")) {
        r.l = jo_new_object("android/content/SharedPreferences$Editor");
    } else if (is(name, "putString")) {
        char *v = jo_jstring_to_utf8((jstring)args[1].l);
        if (key) pref_set(key, 's', v ? v : "");
        free(v);
    } else if (is(name, "putInt")) {
        snprintf(buf, sizeof(buf), "%d", args[1].i);
        if (key) pref_set(key, 'i', buf);
    } else if (is(name, "putLong")) {
        snprintf(buf, sizeof(buf), "%lld", (long long)args[1].j);
        if (key) pref_set(key, 'l', buf);
    } else if (is(name, "putFloat")) {
        snprintf(buf, sizeof(buf), "%.9g", args[1].f);
        if (key) pref_set(key, 'f', buf);
    } else if (is(name, "putBoolean")) {
        if (key) pref_set(key, 'b', args[1].z ? "1" : "0");
    } else if (is(name, "remove")) {
        if (key) pref_remove(key);
    } else if (is(name, "clear")) {
        prefs_load();
        for (int i = 0; i < n_prefs; i++) {
            free(prefs[i].value);
            prefs[i].value = NULL;
        }
    } else if (is(name, "apply") || is(name, "commit")) {
        prefs_save();
        r.z = JNI_TRUE;
    }
    // put*/remove/clear return the editor itself
    if (strncmp(name, "put", 3) == 0 || is(name, "remove") || is(name, "clear"))
        r.l = jo_new_object("android/content/SharedPreferences$Editor");
    free(key);
    return r;
}

/*
 * Static fields
 */
jvalue jo_java_static_field(const char *cls, const char *name, const char *sig) {
    jvalue r;
    r.j = 0;
    if (is(cls, "android/os/Build$VERSION")) {
        if (is(name, "SDK_INT")) r.i = 28;
        else if (is(name, "RELEASE")) r.l = jstr("9");
        else if (is(name, "INCREMENTAL")) r.l = jstr("1");
        else if (is(name, "CODENAME")) r.l = jstr("REL");
        else if (is(name, "PREVIEW_SDK_INT")) r.i = 0;
    } else if (is(cls, "android/os/Build")) {
        if (is(name, "MODEL")) r.l = jstr("PS Vita");
        else if (is(name, "MANUFACTURER") || is(name, "BRAND")) r.l = jstr("Sony");
        else if (is(name, "DEVICE") || is(name, "PRODUCT") || is(name, "HARDWARE") || is(name, "BOARD")) r.l = jstr("vita");
        else if (is(name, "FINGERPRINT")) r.l = jstr("Sony/vita/vita:9/PSVITA/1:user/release-keys");
        else if (is(name, "TAGS")) r.l = jstr("release-keys");
        else if (is(name, "TYPE")) r.l = jstr("user");
        else if (is(name, "CPU_ABI")) r.l = jstr("armeabi-v7a");
        else if (sig[0] == 'L') r.l = jstr("");
    } else if (is(name, "SDK_INT")) {
        r.i = 28;
    } else if (sig[0] == 'L' && is(sig, "Ljava/lang/String;")) {
        // Constants like Context.WINDOW_SERVICE: the name in lower case is close enough
        char lower[64];
        size_t n = strlen(name);
        if (n >= sizeof(lower)) n = sizeof(lower) - 1;
        for (size_t i = 0; i < n; i++) lower[i] = (name[i] >= 'A' && name[i] <= 'Z') ? name[i] + 32 : name[i];
        lower[n] = 0;
        r.l = jstr(lower);
    } else if (sig[0] == 'L') {
        char cname[128];
        size_t n = strlen(sig);
        if (n > 2 && n - 2 < sizeof(cname)) {
            memcpy(cname, sig + 1, n - 2);
            cname[n - 2] = 0;
            r.l = jo_new_object(cname);
        }
    }
    l_debug("[JAVA] static %s.%s (%s)", cls, name, sig);
    return r;
}

/*
 * Methods
 */
static void log_once(const char *cls, const char *name, const char *sig) {
    static char seen[256][96];
    static int count = 0;
    char key[96];
    snprintf(key, sizeof(key), "%s.%s", cls, name);
    for (int i = 0; i < count; i++)
        if (strcmp(seen[i], key) == 0) return;
    if (count < 256) strcpy(seen[count++], key);
    l_info("[JAVA] default answer for %s.%s%s", cls, name, sig);
}

// Neutral value for the method's return type
static jvalue default_return(const char *sig) {
    jvalue r;
    r.j = 0;
    const char *ret = strchr(sig, ')');
    if (!ret) return r;
    ret++;
    if (is(ret, "Ljava/lang/String;")) {
        r.l = jstr("");
    } else if (is(ret, "Ljava/lang/Class;")) {
        // a jclass, not an object
        r.l = (*env())->FindClass(env(), "java/lang/Object");
    } else if (ret[0] == 'L') {
        char cname[128];
        size_t n = strlen(ret);
        if (n > 2 && n - 2 < sizeof(cname)) {
            memcpy(cname, ret + 1, n - 2);
            cname[n - 2] = 0;
            r.l = jo_new_object(cname);
        }
    }
    return r;
}

/*
 * java.lang.String methods on FalsoJNI strings
 */
static int string_call(const char *name, const char *sig, jobject self, const jvalue *args, int argc, jvalue *r) {
    char *str = jo_jstring_to_utf8((jstring)self);
    if (!str) return 0;
    int len = (int)strlen(str);
    int handled = 1;
    char *a0 = (argc >= 1 && strstr(sig, "(Ljava/lang/String;")) ? jo_jstring_to_utf8((jstring)args[0].l) : NULL;

    if (is(name, "length")) r->i = len;
    else if (is(name, "isEmpty")) r->z = len == 0;
    else if (is(name, "toString") || is(name, "intern") || is(name, "trim")) r->l = self;
    else if (is(name, "hashCode")) {
        int h = 0;
        for (int i = 0; i < len; i++) h = 31 * h + (unsigned char)str[i];
        r->i = h;
    } else if (is(name, "equals")) r->z = a0 && strcmp(str, a0) == 0;
    else if (is(name, "equalsIgnoreCase")) r->z = a0 && strcasecmp(str, a0) == 0;
    else if (is(name, "startsWith")) r->z = a0 && strncmp(str, a0, strlen(a0)) == 0;
    else if (is(name, "endsWith")) r->z = a0 && ends_with(str, a0);
    else if (is(name, "contains")) r->z = a0 && strstr(str, a0) != NULL;
    else if (is(name, "indexOf") && a0) {
        char *f = strstr(str, a0);
        r->i = f ? (int)(f - str) : -1;
    } else if (is(name, "charAt")) r->c = (argc >= 1 && args[0].i >= 0 && args[0].i < len) ? (unsigned char)str[args[0].i] : 0;
    else if (is(name, "substring")) {
        int b = argc >= 1 ? args[0].i : 0, e = argc >= 2 ? args[1].i : len;
        if (b < 0) b = 0;
        if (e > len) e = len;
        if (e < b) e = b;
        str[e] = 0;
        r->l = jstr(str + b);
    } else if (is(name, "toLowerCase") || is(name, "toUpperCase")) {
        for (char *c = str; *c; c++)
            *c = name[2] == 'L' ? (char)tolower((unsigned char)*c) : (char)toupper((unsigned char)*c);
        r->l = jstr(str);
    } else if (is(name, "concat")) {
        char *t = malloc(len + (a0 ? strlen(a0) : 0) + 1);
        strcpy(t, str);
        if (a0) strcat(t, a0);
        r->l = jstr(t);
        free(t);
    } else if (is(name, "getBytes")) {
        jbyteArray b = (*env())->NewByteArray(env(), len);
        if (b) (*env())->SetByteArrayRegion(env(), b, 0, len, (const jbyte *)str);
        r->l = b;
    } else {
        handled = 0;
    }
    free(a0);
    free(str);
    return handled;
}

static const char *self_class(jobject self) {
    return self && jo_is_object(self) ? jo_class_name(self) : "";
}

/*
 * Asset pack state queries, answered from the main loop (see na_java_tick)
 */
#define MAX_PACK_QUERIES 16
static struct { jobject name, callback; } pack_queries[MAX_PACK_QUERIES];
static int n_pack_queries = 0;

void *na_unity_native(const char *name, const char *args); // na_unity.c

static void na_java_post_pack_state(jobject name, jobject callback) {
    if (n_pack_queries < MAX_PACK_QUERIES) {
        pack_queries[n_pack_queries].name = name;
        pack_queries[n_pack_queries].callback = callback;
        n_pack_queries++;
    }
}

void na_java_tick(void) {
    if (!n_pack_queries) return;
    void (*result)(JNIEnv *, jobject, jstring, jint, jint) =
        na_unity_native("nativeStatusQueryResult", "(Ljava/lang/String;II");
    int n = n_pack_queries;
    n_pack_queries = 0;
    for (int i = 0; i < n && result; i++) {
        // AssetPackStatus.COMPLETED (4), AssetPackErrorCode.NO_ERROR (0)
        result(jo_env(), pack_queries[i].callback, (jstring)pack_queries[i].name, 4, 0);
        l_info("[JAVA] asset pack state reported");
    }
}

jvalue jo_java_call(const char *cls, const char *name, const char *sig,
                    jobject self, const jvalue *args, int argc) {
    jvalue r;
    r.j = 0;
    const char *sc = self_class(self);

    // Looper/Handler/Message/Choreographer/proxies (na_looper.c)
    if (na_looper_java_call(cls, name, sig, self, args, argc, &r))
        return r;

    // Input events (na_input.c)
    if (self && jo_is_object(self) && na_input_event_call(name, sig, self, args, argc, &r))
        return r;
    if (!self && na_input_static_call(cls, name, args, argc, &r))
        return r;
    if (strstr(sc, "InputDevice") || strstr(cls, "InputDevice")) {
        if (is(name, "getSources")) r.i = 0x1002 | 0x401 | 0x101;
        else if (is(name, "getName")) r.l = jstr("PS Vita");
        else if (is(name, "getDescriptor")) r.l = jstr("vita-builtin");
        else if (is(name, "getVendorId")) r.i = 0x054c;
        else if (is(name, "getProductId")) r.i = 0x0001;
        else if (is(name, "getId")) r.i = 1;
        else if (is(name, "getDevice")) r.l = jo_new_object("android/view/InputDevice");
        else if (is(name, "getDeviceIds")) r.l = NULL;
        else if (is(name, "getKeyboardType")) r.i = 1;
        else r = default_return(sig);
        return r;
    }

    // SharedPreferences and its Editor
    if (strstr(cls, "SharedPreferences") || strstr(sc, "SharedPreferences"))
        return prefs_call(name, args, argc);

    // Strings are FalsoJNI's (not our objects)
    if (self && !jo_is_object(self) && (is(cls, "java/lang/String") || is(cls, "java/lang/CharSequence") ||
                                        is(cls, "java/lang/Object"))) {
        if (string_call(name, sig, self, args, argc, &r)) return r;
    }

    // Constructors
    if (is(name, "<init>")) {
        if (strstr(cls, "Scanner") && argc >= 1 && strncmp(sig, "(Ljava/io/InputStream;", 22) == 0)
            jo_set_obj(self, "__in", args[0].l);
        if (strstr(cls, "java/io/File") && argc >= 1) {
            // File(String) or File(File parent, String child)
            char *a0 = NULL, *a1 = NULL;
            if (strncmp(sig, "(Ljava/lang/String;", 19) == 0) a0 = jo_jstring_to_utf8((jstring)args[0].l);
            else if (strncmp(sig, "(Ljava/io/File;", 15) == 0 && args[0].l) a0 = jo_get_string(args[0].l, "path");
            if (argc >= 2) a1 = jo_jstring_to_utf8((jstring)args[1].l);
            char path[512];
            snprintf(path, sizeof(path), "%s%s%s", a0 ? a0 : "", a1 ? "/" : "", a1 ? a1 : "");
            jo_set_string(self, "path", path);
            free(a0);
            free(a1);
        }
        return r;
    }

    // Context / Activity
    if (is(name, "getPackageName")) {
        r.l = jstr(PACKAGE_NAME);
    } else if (is(name, "getApplicationContext") || is(name, "getBaseContext") || is(name, "getApplication")) {
        r.l = jo_new_object("android/app/Activity");
    } else if (is(name, "getApplicationInfo")) {
        jobject ai = jo_new_object("android/content/pm/ApplicationInfo");
        jo_set_string(ai, "sourceDir", ANDROID_APK_PATH);
        jo_set_string(ai, "publicSourceDir", ANDROID_APK_PATH);
        jo_set_string(ai, "nativeLibraryDir", ANDROID_LIB_DIR_);
        jo_set_string(ai, "dataDir", ANDROID_DATA_DIR);
        jo_set_string(ai, "packageName", PACKAGE_NAME);
        jo_set_int(ai, "targetSdkVersion", 34);
        jo_set_int(ai, "minSdkVersion", 23);
        jo_set_int(ai, "flags", 0);
        r.l = ai;
    } else if (is(name, "getPackageCodePath") || is(name, "getPackageResourcePath")) {
        r.l = jstr(ANDROID_APK_PATH);
    } else if (is(name, "getFilesDir") || is(name, "getNoBackupFilesDir")) {
        r.l = file_object(ANDROID_DATA_DIR "/files");
    } else if (is(name, "getCacheDir") || is(name, "getCodeCacheDir") || is(name, "getExternalCacheDir")) {
        r.l = file_object(ANDROID_DATA_DIR "/cache");
    } else if (is(name, "getExternalFilesDir") || is(name, "getObbDir") || is(name, "getExternalStorageDirectory")) {
        r.l = file_object(ANDROID_EXTERNAL_DIR "/files");
    } else if (is(name, "getDataDir")) {
        r.l = file_object(ANDROID_DATA_DIR);
    } else if (is(name, "getExternalFilesDirs") || is(name, "getObbDirs")) {
        r.l = NULL;
    } else if (is(name, "getExternalStorageState")) {
        r.l = jstr("mounted");
    } else if (is(name, "getSharedPreferences") || is(name, "getDefaultSharedPreferences") || is(name, "getPreferences")) {
        r.l = jo_new_object("android/content/SharedPreferences");

    // java.io.File
    } else if (is(sc, "java/io/File") && (is(name, "getPath") || is(name, "getAbsolutePath") ||
                                          is(name, "getCanonicalPath") || is(name, "toString"))) {
        char *p = jo_get_string(self, "path");
        r.l = jstr(p ? p : "");
        free(p);
    } else if (is(sc, "java/io/File") && is(name, "length")) {
        char *p = jo_get_string(self, "path");
        char mb[512];
        struct stat st;
        r.j = (p && stat(na_apk_map_path(p, mb, sizeof(mb)), &st) == 0) ? (jlong)st.st_size : 0;
        free(p);
    } else if (is(sc, "java/io/File") && is(name, "exists")) {
        char *p = jo_get_string(self, "path");
        char mb[512];
        struct stat st;
        r.z = (p && stat(na_apk_map_path(p, mb, sizeof(mb)), &st) == 0) ? JNI_TRUE : JNI_FALSE;
        free(p);
    } else if (is(sc, "java/io/File") && (is(name, "isDirectory") || is(name, "mkdirs") || is(name, "mkdir") ||
                                          is(name, "canWrite") || is(name, "canRead"))) {
        r.z = JNI_TRUE;

    // Free space: Unity checks it before copying its data (File.getUsableSpace, StatFs)
    } else if (is(name, "getUsableSpace") || is(name, "getFreeSpace") || is(name, "getAvailableBytes") ||
               is(name, "getFreeBytes")) {
        r.j = 2LL * 1024 * 1024 * 1024;
    } else if (is(name, "getTotalSpace") || is(name, "getTotalBytes")) {
        r.j = 4LL * 1024 * 1024 * 1024;
    } else if (is(name, "getBlockSizeLong") || is(name, "getBlockSize")) {
        if (sig[strlen(sig) - 1] == 'J') r.j = 4096; else r.i = 4096;
    } else if (is(name, "getAvailableBlocksLong") || is(name, "getFreeBlocksLong") || is(name, "getAvailableBlocks") ||
               is(name, "getFreeBlocks")) {
        if (sig[strlen(sig) - 1] == 'J') r.j = 524288; else r.i = 524288;
    } else if (is(name, "getBlockCountLong") || is(name, "getBlockCount")) {
        if (sig[strlen(sig) - 1] == 'J') r.j = 1048576; else r.i = 1048576;
    } else if (is(name, "getDataDirectory") || is(name, "getExternalStorageDirectory") ||
               is(name, "getDownloadCacheDirectory") || is(name, "getRootDirectory")) {
        r.l = file_object(ANDROID_DATA_DIR "/files");

    // PackageManager
    } else if (is(name, "getPackageManager")) {
        r.l = jo_new_object("android/content/pm/PackageManager");
    } else if (is(name, "getPackageInfo")) {
        jobject pi = jo_new_object("android/content/pm/PackageInfo");
        jo_set_string(pi, "packageName", PACKAGE_NAME);
        jo_set_string(pi, "versionName", "1.9.3");
        jo_set_int(pi, "versionCode", 193);
        jo_set_long(pi, "firstInstallTime", 1700000000000LL);
        jo_set_long(pi, "lastUpdateTime", 1700000000000LL);
        r.l = pi;
    } else if (is(name, "getInstallerPackageName")) {
        r.l = jstr("com.android.vending");

    // Locale
    } else if (is(name, "getLanguage")) {
        r.l = jstr("en");
    } else if (is(name, "getCountry")) {
        r.l = jstr("US");
    } else if (is(name, "toLanguageTag")) {
        r.l = jstr("en-US");
    } else if (is(sc, "java/util/Locale") && is(name, "toString")) {
        r.l = jstr("en_US");

    // Display
    } else if (is(name, "getMetrics") || is(name, "getRealMetrics") || is(name, "getDisplayMetrics")) {
        jobject dm = (is(name, "getDisplayMetrics") || argc < 1) ? jo_new_object("android/util/DisplayMetrics") : args[0].l;
        if (dm) {
            jo_set_int(dm, "widthPixels", SCREEN_W);
            jo_set_int(dm, "heightPixels", SCREEN_H);
            jo_set_float(dm, "density", 1.375f);
            jo_set_int(dm, "densityDpi", 220);
            jo_set_float(dm, "scaledDensity", 1.375f);
            jo_set_float(dm, "xdpi", 220.0f);
            jo_set_float(dm, "ydpi", 220.0f);
        }
        r.l = dm;
    } else if (is(name, "getRefreshRate")) {
        r.f = 60.0f;
    } else if (is(name, "getRotation") || is(name, "getOrientation")) {
        r.i = 0;
    } else if (is(name, "getWidth") && strstr(sc, "Display")) {
        r.i = SCREEN_W;
    } else if (is(name, "getHeight") && strstr(sc, "Display")) {
        r.i = SCREEN_H;

    // Assets from the APK: AssetManager.open() -> InputStream, read through Scanner/read()
    } else if (is(name, "open") && (strstr(cls, "AssetManager") || strstr(sc, "AssetManager")) && argc >= 1) {
        char *n = jo_jstring_to_utf8((jstring)args[0].l);
        char path[512];
        snprintf(path, sizeof(path), "assets/%s", n ? n : "");
        size_t size = 0;
        void *data = na_apk_read(path, &size);
        if (data && strcmp(path, "assets/bin/Data/boot.config") == 0) {
            // Fewer job threads (each one costs real memory here) and smaller
            // allocator blocks
            static const char extra[] =
                "\njob-worker-count=2\n"
                "background-job-worker-count=1\n"
                "enlighten-job-worker-count=1\n"
                "memorysetup-main-allocator-block-size=4194304\n"
                "memorysetup-thread-allocator-block-size=2097152\n"
                "memorysetup-gfx-main-allocator-block-size=2097152\n"
                "memorysetup-gfx-thread-allocator-block-size=2097152\n"
                "memorysetup-cache-allocator-block-size=1048576\n"
                "memorysetup-typetree-allocator-block-size=1048576\n";
            char *grown = realloc(data, size + sizeof(extra));
            if (grown) {
                memcpy(grown + size, extra, sizeof(extra));
                size += sizeof(extra) - 1;
                data = grown;
                l_info("[JAVA] boot.config: added job-worker and memory settings");
            }
        }
        l_info("[JAVA] AssetManager.open(%s): %s", n ? n : "?", data ? "found" : "not found");
        free(n);
        if (data) {
            jobject in = jo_new_object("java/io/InputStream");
            jo_set_long(in, "__data", (jlong)(uintptr_t)data);
            jo_set_long(in, "__size", (jlong)size);
            jo_set_long(in, "__pos", 0);
            r.l = in;
        } else {
            r.l = NULL;
        }
    } else if (strstr(sc, "Scanner") && is(name, "useDelimiter")) {
        r.l = self;
    } else if (strstr(sc, "Scanner") && (is(name, "next") || is(name, "nextLine"))) {
        jobject in = jo_get_obj(self, "__in");
        char *d = in ? (char *)(uintptr_t)jo_get_long(in, "__data") : NULL;
        jlong pos = in ? jo_get_long(in, "__pos") : 0, size = in ? jo_get_long(in, "__size") : 0;
        if (d && pos < size) {
            r.l = jstr(d + pos);
            jo_set_long(in, "__pos", size);
        } else {
            r.l = jstr("");
        }
    } else if (strstr(sc, "Scanner") && is(name, "hasNext")) {
        jobject in = jo_get_obj(self, "__in");
        r.z = in && jo_get_long(in, "__pos") < jo_get_long(in, "__size");
    } else if (strstr(sc, "InputStream") && is(name, "available")) {
        r.i = (jint)(jo_get_long(self, "__size") - jo_get_long(self, "__pos"));
    } else if (strstr(sc, "InputStream") && is(name, "read")) {
        uint8_t *d = (uint8_t *)(uintptr_t)jo_get_long(self, "__data");
        jlong pos = jo_get_long(self, "__pos"), size = jo_get_long(self, "__size");
        if (!d || pos >= size) {
            r.i = -1;
        } else if (argc == 0) {
            r.i = d[pos];
            jo_set_long(self, "__pos", pos + 1);
        } else {
            jint off = argc >= 3 ? args[1].i : 0;
            jint len = argc >= 3 ? args[2].i : (*env())->GetArrayLength(env(), (jarray)args[0].l);
            if (len > size - pos) len = (jint)(size - pos);
            (*env())->SetByteArrayRegion(env(), (jbyteArray)args[0].l, off, len, (const jbyte *)(d + pos));
            jo_set_long(self, "__pos", pos + len);
            r.i = len;
        }
    } else if (strstr(sc, "InputStream") && is(name, "skip")) {
        jlong pos = jo_get_long(self, "__pos"), size = jo_get_long(self, "__size");
        jlong n = args[0].j;
        if (n > size - pos) n = size - pos;
        jo_set_long(self, "__pos", pos + n);
        r.j = n;
    } else if (strstr(sc, "InputStream") && is(name, "close")) {
        free((void *)(uintptr_t)jo_get_long(self, "__data"));
        jo_set_long(self, "__data", 0);

    // System services
    } else if (is(name, "getSystemService") && argc >= 1) {
        char *n = jo_jstring_to_utf8((jstring)args[0].l);
        const char *c = "java/lang/Object";
        if (n) {
            if (is(n, "activity")) c = "android/app/ActivityManager";
            else if (is(n, "window")) c = "android/view/WindowManager";
            else if (is(n, "audio")) c = "android/media/AudioManager";
            else if (is(n, "display")) c = "android/hardware/display/DisplayManager";
            else if (is(n, "power")) c = "android/os/PowerManager";
            else if (is(n, "input")) c = "android/hardware/input/InputManager";
            else if (is(n, "vibrator")) c = "android/os/Vibrator";
            else if (is(n, "sensor")) c = "android/hardware/SensorManager";
            else if (is(n, "connectivity")) c = "android/net/ConnectivityManager";
            else if (is(n, "wifi")) c = "android/net/wifi/WifiManager";
            else if (is(n, "accessibility")) c = "android/view/accessibility/AccessibilityManager";
        }
        l_info("[JAVA] getSystemService(%s)", n ? n : "?");
        free(n);
        r.l = jo_new_object(c);
    } else if (is(name, "getMemoryInfo") && argc >= 1 && args[0].l) {
        jo_set_long(args[0].l, "totalMem", 512LL * 1024 * 1024);
        jo_set_long(args[0].l, "availMem", 160LL * 1024 * 1024);
        jo_set_long(args[0].l, "threshold", 32LL * 1024 * 1024);
        jo_set_bool(args[0].l, "lowMemory", JNI_FALSE);
    } else if (is(name, "getMemoryClass")) {
        r.i = 256;
    } else if (is(name, "getLargeMemoryClass")) {
        r.i = 512;
    } else if (is(name, "isLowRamDevice")) {
        r.z = JNI_FALSE;
    } else if (is(name, "getDefaultDisplay") || is(name, "getDisplay")) {
        r.l = jo_new_object("android/view/Display");
    } else if ((is(name, "getSize") || is(name, "getRealSize")) && argc >= 1 && args[0].l) {
        jo_set_int(args[0].l, "x", SCREEN_W);
        jo_set_int(args[0].l, "y", SCREEN_H);
    } else if (is(name, "getMode")) {
        r.l = jo_new_object("android/view/Display$Mode");
    } else if (is(name, "getPhysicalWidth")) {
        r.i = SCREEN_W;
    } else if (is(name, "getPhysicalHeight")) {
        r.i = SCREEN_H;
    } else if (is(name, "getDisplayId") || is(name, "getModeId")) {
        r.i = 0;
    } else if (is(name, "getResources")) {
        r.l = jo_new_object("android/content/res/Resources");
    } else if (is(name, "getConfiguration")) {
        jobject c = jo_new_object("android/content/res/Configuration");
        jo_set_int(c, "orientation", 2);       // landscape
        jo_set_int(c, "screenLayout", 0x13);   // large, long
        jo_set_int(c, "uiMode", 0x11);
        jo_set_int(c, "densityDpi", 220);
        jo_set_float(c, "fontScale", 1.0f);
        jo_set_int(c, "screenWidthDp", 698);
        jo_set_int(c, "screenHeightDp", 395);
        jo_set_int(c, "smallestScreenWidthDp", 395);
        jo_set_int(c, "keyboard", 1);
        jo_set_int(c, "navigation", 2);
        jo_set_int(c, "touchscreen", 3);
        jo_set_obj(c, "locale", jo_new_object("java/util/Locale"));
        r.l = c;
    } else if (is(name, "getProperty") && argc >= 1) {
        char *n = jo_jstring_to_utf8((jstring)args[0].l);
        const char *v = NULL;
        if (n && strstr(n, "OUTPUT_SAMPLE_RATE")) v = "48000";
        else if (n && strstr(n, "OUTPUT_FRAMES_PER_BUFFER")) v = "256";
        free(n);
        r.l = v ? jstr(v) : NULL;
    } else if (is(name, "getStreamVolume")) {
        r.i = 12;
    } else if (is(name, "getStreamMaxVolume")) {
        r.i = 15;
    } else if (is(name, "requestAudioFocus")) {
        r.i = 1; // AUDIOFOCUS_REQUEST_GRANTED
    } else if (is(name, "isMusicActive") || is(name, "isBluetoothA2dpOn") || is(name, "isWiredHeadsetOn")) {
        r.z = JNI_FALSE;
    } else if (is(name, "getString") && strstr(cls, "Settings")) {
        char *n = argc >= 2 ? jo_jstring_to_utf8((jstring)args[1].l) : NULL;
        r.l = (n && is(n, "android_id")) ? jstr("5649544130303031") : NULL;
        free(n);
    } else if (is(name, "getInt") && strstr(cls, "Settings")) {
        r.i = argc >= 3 ? args[2].i : 0; // the default value
    } else if (is(name, "getFloat") && strstr(cls, "Settings")) {
        r.f = argc >= 3 ? args[2].f : 1.0f;
    } else if (is(name, "isScreenOn") || is(name, "isInteractive")) {
        r.z = JNI_TRUE;
    } else if (is(name, "getIntent")) {
        r.l = jo_new_object("android/content/Intent");
    } else if (is(name, "getStringExtra") && argc >= 1) {
        // The "unity" extra is Unity's command line. Smaller allocator blocks:
        // Unity reserves them up front, and on the Vita reserved memory is
        // real memory (see mmap in reimpl/mem.c).
        char *k = jo_jstring_to_utf8((jstring)args[0].l);
        if (k && is(k, "unity")) {
            r.l = jstr("-memorysetup-main-allocator-block-size=4194304"
                       " -memorysetup-thread-allocator-block-size=2097152"
                       " -memorysetup-gfx-main-allocator-block-size=2097152"
                       " -memorysetup-gfx-thread-allocator-block-size=2097152"
                       " -memorysetup-cache-allocator-block-size=1048576"
                       " -memorysetup-typetree-allocator-block-size=1048576");
            l_info("[JAVA] Intent extra \"unity\": memory setup arguments");
        } else {
            r.l = NULL;
        }
        free(k);
    } else if (is(name, "getDataString") || is(name, "getAction")) {
        r.l = NULL;
    } else if (is(name, "getData") && strstr(sc, "Intent")) {
        r.l = NULL;

    // Device capabilities
    } else if (is(name, "hasSystemFeature")) {
        r.z = JNI_FALSE;
    } else if (is(name, "checkCallingOrSelfPermission") || is(name, "checkSelfPermission") ||
               is(name, "checkPermission")) {
        r.i = 0; // PERMISSION_GRANTED
    } else if (is(name, "isInMultiWindowMode") || is(name, "isInPictureInPictureMode")) {
        r.z = JNI_FALSE;
    } else if (is(name, "hasWindowFocus")) {
        r.z = JNI_TRUE;

    // Lifecycle
    } else if (is(name, "finish") || is(name, "exit") || is(name, "quit") || is(name, "killProcess")) {
        l_info("[JAVA] %s.%s: the game asked to quit", cls, name);
        sceKernelExitProcess(0);

    // java.lang
    } else if (is(name, "hashCode")) {
        r.i = (jint)(uintptr_t)self;
    } else if (is(name, "equals")) {
        r.z = (argc >= 1 && args[0].l == self) ? JNI_TRUE : JNI_FALSE;
    } else if (is(name, "getName") && strstr(cls, "Class")) {
        r.l = jstr(sc[0] ? sc : cls);
    } else if (is(name, "currentThread")) {
        r.l = jo_new_object("java/lang/Thread");
    } else if (is(name, "findLibrary") && argc >= 1) {
        // System.loadLibrary name -> path of the .so (na_modules.c knows the game's libraries)
        char *n = jo_jstring_to_utf8((jstring)args[0].l);
        char path[256];
        snprintf(path, sizeof(path), ANDROID_LIB_DIR_ "/lib%s.so", n ? n : "");
        l_info("[JAVA] findLibrary(%s) -> %s", n ? n : "?", path);
        free(n);
        r.l = jstr(path);
    } else if (is(cls, "android/net/Uri") && (is(name, "encode") || is(name, "decode")) && argc >= 1) {
        // Unity's PlayerPrefs store every key as Uri.encode(key): answered
        // with null, all keys became "" and every value overwrote the last one
        // (the game then saw a value for "intro seen" and skipped intro and tutorial).
        char *in = jo_jstring_to_utf8((jstring)args[0].l);
        if (!in) {
            r.l = NULL;
        } else if (is(name, "encode")) {
            // Android's Uri.encode: everything but A-Z a-z 0-9 _-!.~'()* as %XX (UTF-8 bytes)
            static const char *keep = "_-!.~'()*";
            char *out = malloc(strlen(in) * 3 + 1), *o = out;
            for (const unsigned char *p = (const unsigned char *)in; *p; p++) {
                if (isalnum(*p) || strchr(keep, *p)) *o++ = (char)*p;
                else o += sprintf(o, "%%%02X", *p);
            }
            *o = 0;
            r.l = jstr(out);
            free(out);
        } else {
            char *out = malloc(strlen(in) + 1), *o = out;
            for (const char *p = in; *p; p++) {
                if (p[0] == '%' && isxdigit((unsigned char)p[1]) && isxdigit((unsigned char)p[2])) {
                    char h[3] = { p[1], p[2], 0 };
                    *o++ = (char)strtol(h, NULL, 16);
                    p += 2;
                } else {
                    *o++ = *p;
                }
            }
            *o = 0;
            r.l = jstr(out);
            free(out);
        }
        free(in);
    } else if (strstr(cls, "PlayAssetDeliveryUnityWrapper")) {
        // No Google Play here: every asset pack is inside the APK. Unity
        // waits for the state of its core packs before loading the game's
        // first real scene, so the query is answered (asynchronously, from
        // the main loop) as "completed".
        if (is(name, "playCoreApiMissing")) {
            r.z = JNI_TRUE;
        } else if (is(name, "init")) {
            r.l = jo_new_object("com/unity3d/player/PlayAssetDeliveryUnityWrapper");
        } else if (is(name, "getAssetPackState") && argc >= 2) {
            char *n = jo_jstring_to_utf8((jstring)args[0].l);
            l_info("[JAVA] getAssetPackState(%s): reporting it installed", n ? n : "?");
            free(n);
            na_java_post_pack_state(args[0].l, args[1].l);
        } else {
            r = default_return(sig);
        }
    } else if (strstr(cls, "AlertDialog") || strstr(sc, "AlertDialog")) {
        // Unity reports fatal errors in a dialog: put the text in the log
        for (int i = 0; i < argc; i++) {
            if (args[i].l && !jo_is_object(args[i].l) && strstr(sig, "CharSequence")) {
                char *t = jo_jstring_to_utf8((jstring)args[i].l);
                if (t && t[0]) l_error("[JAVA] AlertDialog.%s: %s", name, t);
                free(t);
            }
        }
        r = default_return(sig);
    } else if (is(name, "kill")) {
        l_error("[JAVA] %s.kill(): Unity is shutting down (see the dialog text above)", cls);
        sceKernelExitProcess(0);
    } else if (is(name, "getClassLoader")) {
        r.l = jo_new_object("java/lang/ClassLoader");
    } else if ((is(name, "loadClass") || is(name, "forName")) && argc >= 1) {
        char *n = jo_jstring_to_utf8((jstring)args[0].l);
        if (n) {
            for (char *c = n; *c; c++)
                if (*c == '.') *c = '/';
            r.l = (*env())->FindClass(env(), n);
            l_info("[JAVA] loadClass(%s)", n);
            free(n);
        }
    } else {
        log_once(cls[0] ? cls : sc, name, sig);
        r = default_return(sig);
    }
    return r;
}
