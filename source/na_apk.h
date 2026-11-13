/*
 * Copyright (C) 2026 Vita port contributors
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

/**
 * @file  na_apk.h
 * @brief Reading files from base.apk.
 */

#ifndef NA_APK_H
#define NA_APK_H

#include <stddef.h>

/** Whether the APK contains this path (e.g. "assets/bin/Data/boot.config"). */
int na_apk_exists(const char *name);

/** Reads a file from the APK into a malloc'd, NUL-terminated buffer, or NULL. */
void *na_apk_read(const char *name, size_t *size);

/**
 * Paths inside the APK ("<DATA_PATH>base.apk/assets/...") map to
 * DATA_PATH "assets/..." (extracted on first use). Other paths are returned
 * unchanged. buf receives the mapped path.
 */
const char *na_apk_map_path(const char *path, char *buf, size_t buflen);

/** Extracts every APK entry starting with prefix into out_dir (+ rest of the name). */
int na_apk_extract_prefix(const char *prefix, const char *out_dir);

/** Undoes "base/ux0:..." concatenations (returns a pointer into path). */
const char *na_fix_path(const char *path);

/*
 * The paths Unity is told about (Java side) are Android paths, so that they
 * start with '/' (Unity treats anything else as relative). They are mapped
 * back to the Vita in na_apk_map_path().
 */
#define ANDROID_DATA_DIR "/data/user/0/" PACKAGE_NAME
#define ANDROID_APP_DIR "/data/app/" PACKAGE_NAME "-1"
#define ANDROID_APK_PATH ANDROID_APP_DIR "/base.apk"
#define ANDROID_LIB_DIR_ ANDROID_APP_DIR "/lib/arm"
#define ANDROID_EXTERNAL_DIR "/storage/emulated/0/Android/data/" PACKAGE_NAME

#endif // NA_APK_H
