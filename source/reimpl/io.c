/*
 * Copyright (C) 2021      Andy Nguyen
 * Copyright (C) 2022      Rinnegatamante
 * Copyright (C) 2022-2024 Volodymyr Atamanenko
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

#include "reimpl/io.h"


#include <string.h>
#include <sys/stat.h>
#include <sys/unistd.h>
#include <stdlib.h>
#include <dirent.h>
#include <stdarg.h>
#include <psp2/kernel/threadmgr.h>

#ifdef USE_SCELIBC_IO
#include <libc_bridge/libc_bridge.h>
#endif

#include "utils/logger.h"
#include "utils/utils.h"

// Includes the following inline utilities:
// int oflags_musl_to_newlib(int flags);
// dirent64_bionic * dirent_newlib_to_bionic(struct dirent* dirent_newlib);
// void stat_newlib_to_bionic(struct stat * src, stat64_bionic * dst);
#include "reimpl/bits/_struct_converters.c"

// First file accesses go to the log (always on), to see what the game loads
static int io_logged = 0;
#define l_io(...) do { if (io_logged < 400) { io_logged++; l_info(__VA_ARGS__); } } while (0)


#include "na_apk.h"
#define MAP_PATH(p) char mapped_##p[512]; p = na_apk_map_path(p, mapped_##p, sizeof(mapped_##p))


/*
 * Asset file tracing: open/read/lseek/close on Unity's data files and the
 * APK are logged (with the thread), to find where big textures lose data.
 */
#define MAX_TRACK_FD 256
static char *fd_names[MAX_TRACK_FD];
volatile int na_io_trace_left = 0; // 4000 to trace asset I/O again

// the whole path, and a number that changes each time the fd is opened or
// closed (na_filecache.c keeps what it found out about an open file by it)
static char *fd_paths[MAX_TRACK_FD];
static uint32_t fd_gens[MAX_TRACK_FD];

const char *na_io_fd_name(int fd) {
    return (fd >= 0 && fd < MAX_TRACK_FD) ? fd_names[fd] : NULL;
}

const char *na_io_fd_path(int fd, uint32_t *gen) {
    if (fd < 0 || fd >= MAX_TRACK_FD) return NULL;
    if (gen) *gen = fd_gens[fd];
    return fd_paths[fd];
}

static void track_fd(int fd, const char *path) {
    if (fd < 0 || fd >= MAX_TRACK_FD) return;
    free(fd_names[fd]);
    free(fd_paths[fd]);
    fd_names[fd] = NULL;
    fd_paths[fd] = NULL;
    fd_gens[fd]++;
    if (path && (strstr(path, "/Data/") || strstr(path, "base.apk"))) {
        const char *b = strrchr(path, '/');
        fd_names[fd] = strdup(b ? b + 1 : path);
        fd_paths[fd] = strdup(path);
    }
}

FILE * fopen_soloader(const char * filename, const char * mode) {
    MAP_PATH(filename);
    // Three cores for applications at 444 MHz
    if (strcmp(filename, "/sys/devices/system/cpu/present") == 0 ||
        strcmp(filename, "/sys/devices/system/cpu/possible") == 0) {
        return fopen_soloader("app0:/cpu_present", mode);
    } else if (strstr(filename, "/sys/devices/system/cpu/cpu") == filename && strstr(filename, "cpuinfo_max_freq")) {
        return fopen_soloader("app0:/cpu_max_freq", mode);
    } else if (strcmp(filename, "/proc/cpuinfo") == 0) {
        return fopen_soloader("app0:/cpuinfo", mode);
    } else if (strcmp(filename, "/proc/meminfo") == 0) {
        return fopen_soloader("app0:/meminfo", mode);
    }

#ifdef USE_SCELIBC_IO
    FILE* ret = sceLibcBridge_fopen(filename, mode);
#else
    FILE* ret = fopen(filename, mode);
#endif

    if (ret) {
        if (strncmp(filename, "app0:/cpu", 9) != 0) l_io("fopen(%s, %s): %p", filename, mode, ret);
    } else {
        // Unity polls the /sys cpu files every frame: report missing ones once
        static int sys_misses = 0;
        if (strncmp(filename, "/sys/", 5) != 0 || sys_misses++ < 4)
            l_warn("fopen(%s, %s): %p", filename, mode, ret);
    }

    return ret;
}

int open_soloader(const char * path, int oflag, ...) {
    MAP_PATH(path);
    if (strcmp(path, "/sys/devices/system/cpu/present") == 0 ||
        strcmp(path, "/sys/devices/system/cpu/possible") == 0) {
        return open_soloader("app0:/cpu_present", oflag);
    } else if (strstr(path, "/sys/devices/system/cpu/cpu") == path && strstr(path, "cpuinfo_max_freq")) {
        return open_soloader("app0:/cpu_max_freq", oflag);
    } else if (strcmp(path, "/proc/cpuinfo") == 0) {
        return open_soloader("app0:/cpuinfo", oflag);
    } else if (strcmp(path, "/proc/meminfo") == 0) {
        return open_soloader("app0:/meminfo", oflag);
    } else if (strcmp(path, "/dev/urandom") == 0) {
        return open_soloader("app0:/urandom", oflag);
    }

    mode_t mode = 0666;
    if (((oflag & BIONIC_O_CREAT) == BIONIC_O_CREAT) ||
        ((oflag & BIONIC_O_TMPFILE) == BIONIC_O_TMPFILE)) {
        va_list args;
        va_start(args, oflag);
        mode = (mode_t)(va_arg(args, int));
        va_end(args);
    }

    oflag = oflags_bionic_to_newlib(oflag);
    int ret = open(path, oflag, mode);
    track_fd(ret, path);
    if (ret >= 0 && fd_names[ret] && na_io_trace_left > 0) {
        na_io_trace_left--;
        l_info("[IO] t%x open(%s): %d", sceKernelGetThreadId(), path, ret);
    } else if (ret >= 0)
        l_io("open(%s, %x): %i", path, oflag, ret);
    else
        l_warn("open(%s, %x): %i", path, oflag, ret);
    return ret;
}

int fstat_soloader(int fd, stat64_bionic * buf) {
    struct stat st;
    int res = fstat(fd, &st);

    if (res == 0)
        stat_newlib_to_bionic(&st, buf);

    if (res != 0) l_warn("fstat(%i): %i", fd, res);
    return res;
}

int stat_soloader(const char * path, stat64_bionic * buf) {
    MAP_PATH(path);
    if (strcmp(path, "/system/lib/libOpenSLES.so") == 0) {
        l_debug("stat(%s): returning 0 in case this is a check for OpenSLES support", path);
        return 0;
    }

    struct stat st;
    int res = stat(path, &st);

    if (res == 0)
        stat_newlib_to_bionic(&st, buf);

    l_io("stat(%s): %i", path, res);
    return res;
}

int fclose_soloader(FILE * f) {
#ifdef USE_SCELIBC_IO
    int ret = sceLibcBridge_fclose(f);
#else
    int ret = fclose(f);
#endif

    l_debug("fclose(%p): %i", f, ret);
    return ret;
}

int close_soloader(int fd) {
    if (na_io_fd_name(fd) && na_io_trace_left > 0) {
        na_io_trace_left--;
        l_info("[IO] t%x close(%d %s)", sceKernelGetThreadId(), fd, na_io_fd_name(fd));
    }
    track_fd(fd, NULL);
    int ret = close(fd);
    l_debug("close(%i): %i", fd, ret);
    return ret;
}

DIR* opendir_soloader(char* _pathname) {
    const char *path = _pathname;
    MAP_PATH(path);
    DIR* ret = opendir(path);
    l_debug("opendir(\"%s\"): %p", _pathname, ret);
    return ret;
}

struct dirent64_bionic * readdir_soloader(DIR * dir) {
    static struct dirent64_bionic dirent_tmp;

    struct dirent* ret = readdir(dir);
    l_debug("readdir(%p): %p", dir, ret);

    if (ret) {
        dirent64_bionic* entry_tmp = dirent_newlib_to_bionic(ret);
        memcpy(&dirent_tmp, entry_tmp, sizeof(dirent64_bionic));
        free(entry_tmp);
        return &dirent_tmp;
    }

    return NULL;
}

int readdir_r_soloader(DIR * dirp, dirent64_bionic * entry,
                       dirent64_bionic ** result) {
    struct dirent dirent_tmp;
    struct dirent * pdirent_tmp;

    int ret = readdir_r(dirp, &dirent_tmp, &pdirent_tmp);

    if (ret == 0) {
        dirent64_bionic* entry_tmp = dirent_newlib_to_bionic(&dirent_tmp);
        memcpy(entry, entry_tmp, sizeof(dirent64_bionic));
        *result = (pdirent_tmp != NULL) ? entry : NULL;
        free(entry_tmp);
    }

    l_debug("readdir_r(%p, %p, %p): %i", dirp, entry, result, ret);
    return ret;
}

int closedir_soloader(DIR * dir) {
    int ret = closedir(dir);
    l_debug("closedir(%p): %i", dir, ret);
    return ret;
}

int fcntl_soloader(int fd, int cmd, ...) {
    l_warn("fcntl(%i, %i, ...): not implemented", fd, cmd);
    return 0;
}

int ioctl_soloader(int fd, int request, ...) {
    l_warn("ioctl(%i, %i, ...): not implemented", fd, request);
    return 0;
}

int fsync_soloader(int fd) {
    int ret = fsync(fd);
    l_debug("fsync(%i): %i", fd, ret);
    return ret;
}

int access_soloader(const char *path, int mode) {
    MAP_PATH(path);
    struct stat st;
    int r = stat(path, &st);
    l_io("access(%s): %i", path, r);
    return r;
}
