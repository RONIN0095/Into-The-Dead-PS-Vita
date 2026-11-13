/*
 * Copyright (C) 2021      Andy Nguyen
 * Copyright (C) 2022      Rinnegatamante
 * Copyright (C) 2022-2024 Volodymyr Atamanenko
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

/**
 * @file  io.h
 * @brief Wrappers and implementations for some of the IO functions.
 */

#ifndef SOLOADER_IO_H
#define SOLOADER_IO_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include <stdio.h>
#include <sys/dirent.h>
#include <sys/syslimits.h>
#include <sys/fcntl.h>

#ifndef PATH_MAX
#define PATH_MAX 1024
#endif

#ifndef DT_DIR
#define DT_UNKNOWN 0
#define DT_FIFO 1
#define DT_CHR 2
#define DT_DIR 4
#define DT_BLK 6
#define DT_REG 8
#define DT_LNK 10
#define DT_SOCK 12
#define DT_WHT 14
#endif

// bionic's struct stat on 32-bit ARM (= stat64), natural alignment, 104 bytes.
typedef struct stat64_bionic {
    uint64_t st_dev;          // 0
    uint8_t __pad0[4];        // 8
    uint32_t __st_ino;        // 12
    uint32_t st_mode;         // 16
    uint32_t st_nlink;        // 20
    uint32_t st_uid;          // 24
    uint32_t st_gid;          // 28
    uint64_t st_rdev;         // 32
    uint8_t __pad3[4];        // 40
    int64_t st_size;          // 48
    uint32_t st_blksize;      // 56
    uint64_t st_blocks;       // 64
    struct { int32_t tv_sec; int32_t tv_nsec; } st_atim, st_mtim, st_ctim; // 72, 80, 88
    uint64_t st_ino;          // 96
} stat64_bionic;

_Static_assert(sizeof(stat64_bionic) == 104, "bionic struct stat size");
_Static_assert(__builtin_offsetof(stat64_bionic, st_size) == 48, "bionic st_size offset");
_Static_assert(__builtin_offsetof(stat64_bionic, st_blocks) == 64, "bionic st_blocks offset");
_Static_assert(__builtin_offsetof(stat64_bionic, st_ino) == 96, "bionic st_ino offset");

// bionic's struct dirent (= dirent64): 8+8+2+1+256, padded to 280 bytes.
typedef struct dirent64_bionic {
    uint64_t d_ino;           // 0
    int64_t d_off;            // 8
    uint16_t d_reclen;        // 16
    uint8_t d_type;           // 18
    char d_name[256];         // 19
} dirent64_bionic;

_Static_assert(__builtin_offsetof(dirent64_bionic, d_name) == 19, "bionic d_name offset");

int open_soloader(const char * path, int oflag, ...);

FILE * fopen_soloader(const char * filename, const char * mode);

DIR *opendir_soloader(char *name);

int stat_soloader(const char * path, stat64_bionic * buf);

int fstat_soloader(int fd, stat64_bionic * buf);

struct dirent64_bionic * readdir_soloader(DIR *dir);

int readdir_r_soloader(DIR * dirp, dirent64_bionic * entry,
                       dirent64_bionic ** result);

int close_soloader(int fd);

int fclose_soloader(FILE *f);

int closedir_soloader(DIR *dir);

int fcntl_soloader(int fd, int cmd, ...);

int ioctl_soloader(int fd, int request, ... /* arg */);

int fsync_soloader(int fd);
int access_soloader(const char *path, int mode);

#ifdef __cplusplus
};
#endif

#endif // SOLOADER_IO_H
