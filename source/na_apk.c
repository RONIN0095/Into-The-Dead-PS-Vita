/*
 * Copyright (C) 2026 Vita port contributors
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

/**
 * @file  na_apk.c
 * @brief Reading single files out of base.apk (a ZIP archive).
 *
 * Unity reads its data from the APK itself; this is only for the few files
 * the Java side hands out (AssetManager.open() in na_java.c). The central
 * directory is read once and kept in memory.
 */

#include "na_apk.h"

void na_filecache_drop(void); // na_filecache.c

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <zlib.h>
#include <sys/stat.h>
#include <psp2/io/stat.h>
#include <psp2/kernel/threadmgr.h>

#include "utils/logger.h"

#define APK_PATH DATA_PATH "base.apk"

typedef struct {
    char *name;
    uint32_t local_offset;
    uint32_t comp_size;
    uint32_t size;
    uint32_t crc;
    uint16_t method;
    uint8_t verified; // the extracted copy's CRC was checked
} apk_entry;

static apk_entry *entries = NULL;
static int n_entries = 0;
static int loaded = 0;

/*
 * Extracted copies of game data are checked against the APK's CRC32 once:
 * on the test Vita the asset split files had the right sizes but were
 * mostly zeros (an extraction that stopped half way, or a bad copy), so the
 * title picture, cutscenes and level scenery loaded as black or flat colour.
 * Files that fail the check are extracted again.
 */
#define CRC_OK_PATH DATA_PATH "crc_ok.txt"

static uint32_t file_crc(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    static uint8_t buf[128 * 1024];
    uLong c = crc32(0L, Z_NULL, 0);
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) c = crc32(c, buf, n);
    fclose(f);
    return (uint32_t)c;
}

static uint16_t rd16(const uint8_t *p) { return p[0] | (p[1] << 8); }
static uint32_t rd32(const uint8_t *p) { return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24); }

static int load_directory(void) {
    if (loaded) return n_entries > 0;
    loaded = 1;
    FILE *f = fopen(APK_PATH, "rb");
    if (!f) {
        l_error("[apk] cannot open %s", APK_PATH);
        return 0;
    }
    // End of central directory: in the last 64 KB + 22 bytes
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    long tail = size < 65557 ? size : 65557;
    uint8_t *buf = malloc(tail);
    fseek(f, size - tail, SEEK_SET);
    fread(buf, 1, tail, f);
    long eocd = -1;
    for (long i = tail - 22; i >= 0; i--)
        if (rd32(buf + i) == 0x06054b50) { eocd = i; break; }
    if (eocd < 0) {
        l_error("[apk] no end of central directory");
        free(buf);
        fclose(f);
        return 0;
    }
    uint16_t count = rd16(buf + eocd + 10);
    uint32_t cd_size = rd32(buf + eocd + 12);
    uint32_t cd_off = rd32(buf + eocd + 16);
    free(buf);

    uint8_t *cd = malloc(cd_size);
    fseek(f, cd_off, SEEK_SET);
    if (fread(cd, 1, cd_size, f) != cd_size) {
        free(cd);
        fclose(f);
        return 0;
    }
    fclose(f);

    entries = calloc(count, sizeof(apk_entry));
    const uint8_t *p = cd;
    for (int i = 0; i < count && p + 46 <= cd + cd_size; i++) {
        if (rd32(p) != 0x02014b50) break;
        uint16_t name_len = rd16(p + 28), extra = rd16(p + 30), comment = rd16(p + 32);
        apk_entry *e = &entries[n_entries++];
        e->method = rd16(p + 10);
        e->comp_size = rd32(p + 20);
        e->size = rd32(p + 24);
        e->crc = rd32(p + 16);
        e->local_offset = rd32(p + 42);
        e->name = malloc(name_len + 1);
        memcpy(e->name, p + 46, name_len);
        e->name[name_len] = 0;
        p += 46 + name_len + extra + comment;
    }
    free(cd);
    l_info("[apk] %d entries in %s", n_entries, APK_PATH);
    // extracted files already checked in earlier runs
    FILE *ok = fopen(CRC_OK_PATH, "r");
    if (ok) {
        char line[512];
        int n = 0;
        while (fgets(line, sizeof(line), ok)) {
            line[strcspn(line, "\r\n")] = 0;
            for (int i = 0; i < n_entries; i++)
                if (strcmp(entries[i].name, line) == 0) {
                    entries[i].verified = 1;
                    n++;
                    break;
                }
        }
        fclose(ok);
        l_info("[apk] %d extracted files checked before", n);
    }
    return n_entries > 0;
}

static apk_entry *find(const char *name) {
    if (!load_directory()) return NULL;
    for (int i = 0; i < n_entries; i++)
        if (strcmp(entries[i].name, name) == 0) return &entries[i];
    return NULL;
}

int na_apk_exists(const char *name) {
    return find(name) != NULL;
}

void *na_apk_read(const char *name, size_t *size_out) {
    apk_entry *e = find(name);
    if (!e) return NULL;
    FILE *f = fopen(APK_PATH, "rb");
    if (!f) return NULL;
    uint8_t lh[30];
    fseek(f, e->local_offset, SEEK_SET);
    if (fread(lh, 1, 30, f) != 30 || rd32(lh) != 0x04034b50) {
        fclose(f);
        return NULL;
    }
    fseek(f, e->local_offset + 30 + rd16(lh + 26) + rd16(lh + 28), SEEK_SET);
    uint8_t *comp = malloc(e->comp_size + 1);
    size_t got = fread(comp, 1, e->comp_size, f);
    fclose(f);
    if (got != e->comp_size) {
        free(comp);
        return NULL;
    }

    uint8_t *out;
    if (e->method == 0) {
        out = comp;
    } else if (e->method == 8) {
        out = malloc(e->size + 1);
        z_stream zs;
        memset(&zs, 0, sizeof(zs));
        inflateInit2(&zs, -MAX_WBITS);
        zs.next_in = comp;
        zs.avail_in = e->comp_size;
        zs.next_out = out;
        zs.avail_out = e->size;
        int r = inflate(&zs, Z_FINISH);
        inflateEnd(&zs);
        free(comp);
        if (r != Z_STREAM_END) {
            free(out);
            l_error("[apk] inflate %s failed (%d)", name, r);
            return NULL;
        }
    } else {
        free(comp);
        l_error("[apk] %s: unsupported compression %d", name, e->method);
        return NULL;
    }
    out[e->size] = 0; // handy for text files
    if (size_out) *size_out = e->size;
    return out;
}

/*
 * Paths inside the APK
 *
 * Unity opens its data as "<apk path>/assets/bin/Data/..." through the C
 * library. Such paths are redirected to DATA_PATH "assets/...", and a file
 * that is not there yet is extracted from the APK on first use (streamed,
 * so big files need little memory). The first start is slower because of
 * that; later starts read the extracted files directly.
 */
static SceKernelLwMutexWork map_lock;
static int map_lock_ready = 0;

static void mkdirs(const char *path) {
    char tmp[512];
    snprintf(tmp, sizeof(tmp), "%s", path);
    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = 0;
            sceIoMkdir(tmp, 0777);
            *p = '/';
        }
    }
    sceIoMkdir(tmp, 0777);
}

static int is_dir_in_apk(const char *name) {
    size_t n = strlen(name);
    for (int i = 0; i < n_entries; i++)
        if (strncmp(entries[i].name, name, n) == 0 && entries[i].name[n] == '/') return 1;
    return 0;
}

static int extract(apk_entry *e, const char *out_path) {
    FILE *in = fopen(APK_PATH, "rb");
    if (!in) return -1;
    uint8_t lh[30];
    fseek(in, e->local_offset, SEEK_SET);
    if (fread(lh, 1, 30, in) != 30 || rd32(lh) != 0x04034b50) {
        fclose(in);
        return -1;
    }
    fseek(in, e->local_offset + 30 + rd16(lh + 26) + rd16(lh + 28), SEEK_SET);

    char dir[512];
    snprintf(dir, sizeof(dir), "%s", out_path);
    char *slash = strrchr(dir, '/');
    if (slash) {
        *slash = 0;
        mkdirs(dir);
    }
    char tmp_path[520];
    snprintf(tmp_path, sizeof(tmp_path), "%s.part", out_path);
    FILE *out = fopen(tmp_path, "wb");
    if (!out) {
        fclose(in);
        return -1;
    }

    enum { CHUNK = 128 * 1024 };
    uint8_t *ibuf = malloc(CHUNK), *obuf = malloc(CHUNK);
    uint32_t left = e->comp_size;
    int ok = 1;
    uLong crc = crc32(0L, Z_NULL, 0);
    if (e->method == 0) {
        while (left && ok) {
            size_t n = left < CHUNK ? left : CHUNK;
            if (fread(ibuf, 1, n, in) != n || fwrite(ibuf, 1, n, out) != n) ok = 0;
            crc = crc32(crc, ibuf, n);
            left -= n;
        }
    } else if (e->method == 8) {
        z_stream zs;
        memset(&zs, 0, sizeof(zs));
        inflateInit2(&zs, -MAX_WBITS);
        int r = Z_OK;
        while (ok && r != Z_STREAM_END) {
            if (zs.avail_in == 0 && left) {
                size_t n = left < CHUNK ? left : CHUNK;
                if (fread(ibuf, 1, n, in) != n) { ok = 0; break; }
                left -= n;
                zs.next_in = ibuf;
                zs.avail_in = n;
            }
            zs.next_out = obuf;
            zs.avail_out = CHUNK;
            r = inflate(&zs, Z_NO_FLUSH);
            if (r != Z_OK && r != Z_STREAM_END) { ok = 0; break; }
            size_t have = CHUNK - zs.avail_out;
            if (fwrite(obuf, 1, have, out) != have) ok = 0;
            crc = crc32(crc, obuf, have);
            if (r != Z_STREAM_END && zs.avail_in == 0 && left == 0 && have == 0) { ok = 0; break; }
        }
        inflateEnd(&zs);
    } else {
        ok = 0;
    }
    free(ibuf);
    free(obuf);
    fclose(in);
    fclose(out);
    if (ok && (uint32_t)crc != e->crc) {
        l_warn("[apk] %s: extracted CRC %08x, APK has %08x", e->name, (unsigned)crc, (unsigned)e->crc);
        ok = 0;
    }
    if (!ok) {
        remove(tmp_path);
        return -1;
    }
    remove(out_path);
    rename(tmp_path, out_path);
    na_filecache_drop(); // copies of asset files kept in memory
    // A fresh copy is known good: no CRC check of it on later starts (it may
    // be changed)
    if (strncmp(e->name, "assets/", 7) == 0) {
        FILE *okf = fopen(CRC_OK_PATH, "a");
        if (okf) {
            fprintf(okf, "%s\n", e->name);
            fclose(okf);
        }
        e->verified = 1;
    }
    return 0;
}

// Unity treats paths without a leading '/' as relative and prefixes them with
// a base directory: "ux0:data/x/ux0:data/x/files/...". The last device name
// in the path starts the real one.
const char *na_fix_path(const char *path) {
    if (!path) return path;
    const char *best = path;
    for (const char *p = path + 1; *p; p++) {
        if ((p[0] == 'u' && strncmp(p, "ux0:", 4) == 0) || (p[0] == 'a' && strncmp(p, "app0:", 5) == 0) ||
            (p[0] == 'u' && strncmp(p, "ur0:", 4) == 0))
            best = p;
    }
    return best;
}

static const struct { const char *android; const char *vita; } path_map[] = {
    { ANDROID_APK_PATH, DATA_PATH "base.apk" },
    { ANDROID_LIB_DIR_, DATA_PATH "lib" },
    { ANDROID_APP_DIR, DATA_PATH "app" },
    { ANDROID_DATA_DIR, DATA_PATH "" },
    { "/data/data/" PACKAGE_NAME, DATA_PATH "" },
    { ANDROID_EXTERNAL_DIR, DATA_PATH "external" },
};

const char *na_apk_map_path(const char *path, char *buf, size_t buflen) {
    static const char prefix[] = APK_PATH "/";
    path = na_fix_path(path);
    if (path && path[0] == '/') {
        for (unsigned i = 0; i < sizeof(path_map) / sizeof(path_map[0]); i++) {
            size_t n = strlen(path_map[i].android);
            if (strncmp(path, path_map[i].android, n) == 0 && (path[n] == 0 || path[n] == '/')) {
                char tmp[512];
                const char *rest = path + n;
                const char *vita = path_map[i].vita;
                size_t vl = strlen(vita);
                // DATA_PATH ends in '/': avoid "//"
                if (vl && vita[vl - 1] == '/' && rest[0] == '/') rest++;
                snprintf(tmp, sizeof(tmp), "%s%s", vita, rest);
                if (strlen(tmp) == vl && vl > 1 && tmp[vl - 1] == '/') tmp[vl - 1] = 0; // the data dir itself
                snprintf(buf, buflen, "%s", tmp);
                path = buf;
                break;
            }
        }
    }
    if (!path || strncmp(path, prefix, sizeof(prefix) - 1) != 0)
        return path;
    char name_copy[512];
    snprintf(name_copy, sizeof(name_copy), "%s", path + sizeof(prefix) - 1);
    const char *name = name_copy;
    while (*name == '/') name++;
    snprintf(buf, buflen, DATA_PATH "%s", name);
    size_t l = strlen(buf);
    while (l > 0 && buf[l - 1] == '/') buf[--l] = 0;

    if (!map_lock_ready) {
        sceKernelCreateLwMutex(&map_lock, "apk_map", SCE_KERNEL_MUTEX_ATTR_RECURSIVE, 0, NULL);
        map_lock_ready = 1;
    }
    sceKernelLockLwMutex(&map_lock, 1, NULL);
    if (load_directory()) {
        char entry[512];
        snprintf(entry, sizeof(entry), "%s", name);
        size_t el = strlen(entry);
        while (el > 0 && entry[el - 1] == '/') entry[--el] = 0;
        apk_entry *e = find(entry);
        struct stat st;
        char split[520];
        snprintf(split, sizeof(split), "%s.split0", entry);
        // Big files are stored as name.split0, .split1, ...: Unity looks for
        // the whole file first and then reads the parts itself. (The parts
        // used to be joined into the whole file here as well: seconds of
        // copying on the first visit of a level, for a file nobody read.)
        if (!e && find(split)) {
            sceKernelUnlockLwMutex(&map_lock, 1);
            return buf;
        }
        if (e) {
            if (stat(buf, &st) != 0 || (uint32_t)st.st_size != e->size) {
                l_info("[apk] extracting %s (%u KB)", entry, (unsigned)(e->size / 1024));
                if (extract(e, buf) < 0) l_error("[apk] extracting %s failed", entry);
            } else if (!e->verified && strncmp(entry, "assets/", 7) == 0) {
                uint32_t c = file_crc(buf);
                if (c != e->crc) {
                    l_warn("[apk] %s: CRC %08x, APK has %08x: extracting again", entry, (unsigned)c, (unsigned)e->crc);
                    if (extract(e, buf) < 0) l_error("[apk] extracting %s failed", entry);
                    c = file_crc(buf);
                }
                if (c == e->crc) {
                    FILE *ok = fopen(CRC_OK_PATH, "a");
                    if (ok) {
                        fprintf(ok, "%s\n", entry);
                        fclose(ok);
                    }
                }
            }
            e->verified = 1;
        } else if (is_dir_in_apk(entry)) {
            if (stat(buf, &st) != 0) {
                mkdirs(buf);
                // Small directories (Managed, Metadata, ...) are extracted
                // whole, in case Unity lists and copies them
                size_t n = strlen(entry), total = 0;
                int count = 0;
                for (int i = 0; i < n_entries; i++)
                    if (strncmp(entries[i].name, entry, n) == 0 && entries[i].name[n] == '/') {
                        total += entries[i].size;
                        count++;
                    }
                if (count <= 64 && total <= 32 * 1024 * 1024) {
                    for (int i = 0; i < n_entries; i++) {
                        if (strncmp(entries[i].name, entry, n) != 0 || entries[i].name[n] != '/') continue;
                        char out[512];
                        snprintf(out, sizeof(out), DATA_PATH "%s", entries[i].name);
                        if (stat(out, &st) == 0 && (uint32_t)st.st_size == entries[i].size) continue;
                        l_info("[apk] extracting %s (%u KB)", entries[i].name, (unsigned)(entries[i].size / 1024));
                        extract(&entries[i], out);
                    }
                }
            }
        }
    }
    sceKernelUnlockLwMutex(&map_lock, 1);
    return buf;
}

int na_apk_extract_prefix(const char *prefix, const char *out_dir) {
    if (!map_lock_ready) {
        sceKernelCreateLwMutex(&map_lock, "apk_map", SCE_KERNEL_MUTEX_ATTR_RECURSIVE, 0, NULL);
        map_lock_ready = 1;
    }
    sceKernelLockLwMutex(&map_lock, 1, NULL);
    int n_done = 0;
    if (load_directory()) {
        size_t pl = strlen(prefix);
        for (int i = 0; i < n_entries; i++) {
            if (strncmp(entries[i].name, prefix, pl) != 0) continue;
            char out[512];
            snprintf(out, sizeof(out), "%s%s", out_dir, entries[i].name + pl);
            struct stat st;
            if (stat(out, &st) == 0 && (uint32_t)st.st_size == entries[i].size) continue;
            l_info("[apk] %s -> %s", entries[i].name, out);
            if (extract(&entries[i], out) == 0) n_done++;
        }
    }
    sceKernelUnlockLwMutex(&map_lock, 1);
    return n_done;
}
