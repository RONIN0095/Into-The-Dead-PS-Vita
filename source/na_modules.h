/*
 * Copyright (C) 2026 Vita port contributors
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

/**
 * @file  na_modules.h
 * @brief The Android libraries of the game (libunity.so, libil2cpp.so) and
 *        the libdl functions that let them find each other.
 */

#ifndef NA_MODULES_H
#define NA_MODULES_H

#include <stdint.h>
#include <stddef.h>
#include <so_util/so_util.h>

typedef enum {
    NA_LIB_UNITY = 0,
    NA_LIB_IL2CPP,
    NA_LIB_COUNT
} NaLibId;

typedef struct {
    const char *soname;     // "libunity.so"
    const char *path;       // where the loader reads it from
    uintptr_t load_addr;    // fixed load address (kubridge)
    int required;
    // runtime state
    int loaded;
    int initialized;        // .init_array has run (on first dlopen, like bionic)
    so_module mod;
    Elf32_Phdr *phdr;       // our own copy with the file's relative p_vaddr
    int phnum;
} NaLib;

extern NaLib na_libs[NA_LIB_COUNT];

/** Loads, relocates and resolves every library. Shows an error and exits on failure. */
void na_modules_load(void);

/** Runs .init_array of a library once. */
void na_module_init(NaLibId id);

/** Exported symbol of a library, or 0. */
uintptr_t na_module_symbol(NaLibId id, const char *name);

/** Library containing an address, or NULL. */
NaLib *na_module_for_addr(uintptr_t addr);

/*
 * libdl as seen by the game. Handles are NaLib pointers for game libraries
 * and a few fake handles for system libraries (EGL, GLES, OpenSL ES, ...).
 */
void *na_dlopen(const char *filename, int flags);
void *na_dlsym(void *handle, const char *symbol);
int na_dlclose(void *handle);
const char *na_dlerror(void);

typedef struct {
    const char *dli_fname;
    void *dli_fbase;
    const char *dli_sname;
    void *dli_saddr;
} na_Dl_info;

int na_dladdr(const void *addr, na_Dl_info *info);

/** ARM EHABI: the .ARM.exidx table of the library containing pc. */
uintptr_t na_dl_unwind_find_exidx(uintptr_t pc, int *pcount);

/** bionic struct dl_phdr_info (API 30 layout). */
typedef struct {
    Elf32_Addr dlpi_addr;
    const char *dlpi_name;
    const Elf32_Phdr *dlpi_phdr;
    Elf32_Half dlpi_phnum;
    unsigned long long dlpi_adds;
    unsigned long long dlpi_subs;
    size_t dlpi_tls_modid;
    void *dlpi_tls_data;
} na_dl_phdr_info;

int na_dl_iterate_phdr(int (*cb)(na_dl_phdr_info *info, size_t size, void *data), void *data);

#endif // NA_MODULES_H
