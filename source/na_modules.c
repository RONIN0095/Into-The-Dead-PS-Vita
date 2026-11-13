/*
 * Copyright (C) 2026 Vita port contributors
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

/**
 * @file  na_modules.c
 * @brief Loading libunity.so/libil2cpp.so and the libdl functions they use.
 *
 * On Android, libmain.so loads libunity.so, which later loads libil2cpp.so
 * with dlopen() and talks to it through dlsym(). Here both libraries are
 * loaded at fixed addresses at startup; dlopen() only hands out the already
 * loaded module (and runs its constructors the first time, like bionic).
 *
 * libmain.so is not loaded: all it does is call
 * com.unity3d.player.NativeLoader.load(), which the loader does itself.
 *
 * IL2CPP turns every managed exception into a C++ exception, and its unwinder
 * finds the unwind tables through dl_unwind_find_exidx(). The Boehm GC finds
 * the data segments through dl_iterate_phdr(). Both need the real values.
 */

#include "na_modules.h"
#include "na_aaudio.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <psp2/io/fcntl.h>
#include <psp2/kernel/clib.h>
#include <psp2/kernel/sysmem.h>

#include "utils/dialog.h"
#include "utils/logger.h"
#include "utils/utils.h"

void resolve_imports(so_module *mod);
void *na_dynlib_lookup(const char *symbol);
void *na_gl_get_proc(const char *name);

/*
 * Address layout (kubridge allocates at fixed addresses). so_util puts a
 * 64 KB patch arena right below each load address.
 *
 *   0x98100000  libil2cpp.so  ~34.2 MB (text 32.3 MB + data/bss)
 *   0x9A400000  libunity.so   ~18.2 MB (text 17.2 MB + data/bss)
 */
NaLib na_libs[NA_LIB_COUNT] = {
    [NA_LIB_UNITY]  = { "libunity.so",  DATA_PATH "lib/libunity.so",  0x9A400000, 1 },
    [NA_LIB_IL2CPP] = { "libil2cpp.so", DATA_PATH "lib/libil2cpp.so", 0x98100000, 1 },
};

// Path that Android would report for the libraries (dladdr, /proc/self/maps).
#define ANDROID_LIB_DIR "/data/app/" PACKAGE_NAME "-1/lib/arm/"

static const char *dl_error = NULL;

/*
 * so_util frees the file image after loading, so ELF program headers have
 * to be read separately for dl_iterate_phdr().
 */
static unsigned free_user_mb(void) {
    SceKernelFreeMemorySizeInfo info;
    info.size = sizeof(info);
    if (sceKernelGetFreeMemorySize(&info) < 0) return 0;
    return (unsigned)(info.size_user / (1024 * 1024));
}

static int read_phdrs(NaLib *lib) {
    SceUID fd = sceIoOpen(lib->path, SCE_O_RDONLY, 0);
    if (fd < 0) return -1;
    Elf32_Ehdr eh;
    if (sceIoRead(fd, &eh, sizeof(eh)) != sizeof(eh)) {
        sceIoClose(fd);
        return -1;
    }
    lib->phnum = eh.e_phnum;
    lib->phdr = calloc(eh.e_phnum, sizeof(Elf32_Phdr));
    sceIoLseek(fd, eh.e_phoff, SCE_SEEK_SET);
    int sz = eh.e_phnum * sizeof(Elf32_Phdr);
    int r = sceIoRead(fd, lib->phdr, sz);
    sceIoClose(fd);
    return r == sz ? 0 : -1;
}

void na_modules_load(void) {
    // Load everything first so that so_util can link libraries to each other.
    for (int i = 0; i < NA_LIB_COUNT; i++) {
        NaLib *lib = &na_libs[i];
        if (!file_exists(lib->path)) {
            if (lib->required)
                fatal_error("Missing game file:\n%s\n\nExtract lib/armeabi-v7a/%s from the APK.",
                            lib->path, lib->soname);
            continue;
        }
        l_info("Loading %s at 0x%08x (%u MB free)", lib->soname, lib->load_addr, free_user_mb());
        int r = so_file_load(&lib->mod, lib->path, lib->load_addr);
        if (r < 0)
            fatal_error("Could not load %s (0x%08x).\nFree memory: %u MB.\nIs kubridge installed?",
                        lib->path, r, free_user_mb());
        if (read_phdrs(lib) < 0)
            fatal_error("Could not read the program headers of %s.", lib->path);
        lib->loaded = 1;
        l_info("  text 0x%08x+0x%x, %d data segment(s), exidx 0x%08x (%u entries)",
               lib->mod.text_base, lib->mod.text_size, lib->mod.n_data,
               lib->mod.exidx_base, lib->mod.exidx_size / 8);
    }

    for (int i = 0; i < NA_LIB_COUNT; i++) {
        NaLib *lib = &na_libs[i];
        if (!lib->loaded) continue;
        so_relocate(&lib->mod);
        resolve_imports(&lib->mod);
        so_flush_caches(&lib->mod);
        l_info("%s relocated and linked.", lib->soname);
    }
    l_info("Libraries loaded, %u MB free outside the heap.", free_user_mb());
}

void na_module_init(NaLibId id) {
    NaLib *lib = &na_libs[id];
    if (!lib->loaded || lib->initialized) return;
    lib->initialized = 1;
    l_info("Running constructors of %s (%u)", lib->soname, lib->mod.num_init_array);
    so_initialize(&lib->mod);
    l_info("Constructors of %s done.", lib->soname);
}

uintptr_t na_module_symbol(NaLibId id, const char *name) {
    if (!na_libs[id].loaded) return 0;
    return so_symbol(&na_libs[id].mod, name);
}

NaLib *na_module_for_addr(uintptr_t addr) {
    for (int i = 0; i < NA_LIB_COUNT; i++) {
        NaLib *lib = &na_libs[i];
        if (!lib->loaded) continue;
        if (addr >= lib->mod.text_base && addr < lib->mod.text_base + lib->mod.text_size)
            return lib;
        for (int d = 0; d < lib->mod.n_data; d++)
            if (addr >= lib->mod.data_base[d] && addr < lib->mod.data_base[d] + lib->mod.data_size[d])
                return lib;
    }
    return NULL;
}

/*
 * dlopen / dlsym
 */

// Fake handles for system libraries: their symbols come from the import table.
static const char *system_libs[] = {
    "libc.so", "libm.so", "libdl.so", "liblog.so", "libz.so", "libandroid.so",
    "libEGL.so", "libGLESv2.so", "libGLESv3.so", "libOpenSLES.so", "libmain.so",
    "libaaudio.so", // na_aaudio.c
};
#define N_SYSTEM_LIBS (sizeof(system_libs) / sizeof(system_libs[0]))
static char system_handles[N_SYSTEM_LIBS];

// Libraries the game probes but that must look absent, so it takes another
// path (AAudio -> OpenSL ES, Vulkan -> GLES, ...).
static const char *absent_libs[] = {
    "libvulkan.so", "libmediandk.so", "libcamera2ndk.so",
    "libnativewindow.so", "libapplovin-native-crash-reporter.so", "libswappy.so",
};

#define RTLD_DEFAULT_BIONIC ((void *)0xffffffff)
#define RTLD_NEXT_BIONIC    ((void *)0xfffffffe)

static const char *basename_of(const char *path) {
    const char *s = strrchr(path, '/');
    return s ? s + 1 : path;
}

void *na_dlopen(const char *filename, int flags) {
    if (!filename) {
        // dlopen(NULL): the "main program", i.e. global lookup
        return RTLD_DEFAULT_BIONIC;
    }
    const char *base = basename_of(filename);

    for (int i = 0; i < NA_LIB_COUNT; i++) {
        if (strcmp(base, na_libs[i].soname) == 0) {
            if (!na_libs[i].loaded) break;
            int first = !na_libs[i].initialized;
            na_module_init((NaLibId)i);
            l_info("dlopen(%s) -> %s", filename, na_libs[i].soname);
            // System.loadLibrary() calls JNI_OnLoad; libil2cpp needs it to
            // know the Java VM (AndroidJavaObject & co.)
            if (first && i == NA_LIB_IL2CPP) {
                int (*onload)(void *, void *) = (void *)so_symbol(&na_libs[i].mod, "JNI_OnLoad");
                if (onload) {
                    extern char jvm[];
                    int v = onload(jvm, NULL);
                    l_info("libil2cpp JNI_OnLoad: 0x%x", v);
                }
            }
            return &na_libs[i];
        }
    }
    for (unsigned i = 0; i < N_SYSTEM_LIBS; i++) {
        if (strcmp(base, system_libs[i]) == 0) {
            l_info("dlopen(%s) -> system library", filename);
            return &system_handles[i];
        }
    }
    for (unsigned i = 0; i < sizeof(absent_libs) / sizeof(absent_libs[0]); i++) {
        if (strcmp(base, absent_libs[i]) == 0) {
            l_info("dlopen(%s) -> not available", filename);
            dl_error = "library not available on this device";
            return NULL;
        }
    }
    l_warn("dlopen(%s, 0x%x): unknown library", filename, flags);
    dl_error = "library not found";
    return NULL;
}

/*
 * IL2CPP's garbage collector is off (GC_DONT_GC, see na_dynlib.c). Unity
 * turns it on and triggers collections through these exports; they become
 * no-ops so that no collection ever starts.
 */
static void gc_noop(void) {}
static int gc_is_disabled(void) { return 1; }
static int gc_collect_a_little(void) { return 0; }

// Native stack traces for managed exceptions (Unity adds them to log
// messages): IL2CPP's version walks the ELF notes of the loaded module and
// faults here. An empty trace is enough.
static void native_stack_trace_empty(const void *ex, uintptr_t **addresses, int *count, char **uuid, char **name) {
    if (addresses) *addresses = NULL;
    if (count) *count = 0;
    if (uuid) *uuid = strdup("");
    if (name) *name = strdup("libil2cpp.so");
}

static void *intercept(const char *symbol) {
    if (strcmp(symbol, "il2cpp_native_stack_trace") == 0) {
        l_info("dlsym(%s): empty traces", symbol);
        return (void *)&native_stack_trace_empty;
    }
    if (strncmp(symbol, "il2cpp_gc_", 10) != 0) return NULL;
    const char *s = symbol + 10;
    if (!strcmp(s, "enable") || !strcmp(s, "collect") || !strcmp(s, "start_incremental_collection") ||
        !strcmp(s, "set_mode") || !strcmp(s, "set_max_time_slice_ns")) {
        l_info("dlsym(%s): no-op (GC is off)", symbol);
        return (void *)&gc_noop;
    }
    if (!strcmp(s, "is_disabled")) return (void *)&gc_is_disabled;
    if (!strcmp(s, "collect_a_little")) return (void *)&gc_collect_a_little;
    return NULL;
}

static void *lookup_global(const char *symbol) {
    void *p = na_dynlib_lookup(symbol);
    if (p) return p;
    p = na_gl_get_proc(symbol);
    if (p) return p;
    p = na_aaudio_lookup(symbol);
    if (p) return p;
    for (int i = 0; i < NA_LIB_COUNT; i++) {
        uintptr_t s = na_module_symbol((NaLibId)i, symbol);
        if (s) return (void *)s;
    }
    return NULL;
}

void *na_dlsym(void *handle, const char *symbol) {
    if (!symbol) return NULL;
    void *p = intercept(symbol);
    if (p) return p;

    if (handle >= (void *)&na_libs[0] && handle < (void *)&na_libs[NA_LIB_COUNT]) {
        NaLib *lib = (NaLib *)handle;
        p = (void *)so_symbol(&lib->mod, symbol);
    } else if (handle >= (void *)&system_handles[0] && handle < (void *)&system_handles[N_SYSTEM_LIBS]) {
        p = na_dynlib_lookup(symbol);
        if (!p) p = na_gl_get_proc(symbol);
        if (!p) p = na_aaudio_lookup(symbol);
    } else {
        // RTLD_DEFAULT, RTLD_NEXT or dlopen(NULL)
        p = lookup_global(symbol);
    }

    if (!p) {
        l_warn("dlsym(%p, %s): not found", handle, symbol);
        dl_error = "symbol not found";
    }
    return p;
}

int na_dlclose(void *handle) {
    return 0;
}

const char *na_dlerror(void) {
    const char *e = dl_error;
    dl_error = NULL;
    return e;
}

/*
 * dladdr: name of the closest exported symbol at or below addr.
 */
int na_dladdr(const void *addr, na_Dl_info *info) {
    NaLib *lib = na_module_for_addr((uintptr_t)addr);
    if (!lib || !info) return 0;

    static char fname[NA_LIB_COUNT][128];
    int idx = (int)(lib - na_libs);
    if (!fname[idx][0])
        snprintf(fname[idx], sizeof(fname[idx]), ANDROID_LIB_DIR "%s", lib->soname);

    info->dli_fname = fname[idx];
    info->dli_fbase = (void *)lib->mod.load_addr;
    info->dli_sname = NULL;
    info->dli_saddr = NULL;

    uintptr_t a = (uintptr_t)addr & ~1u, best = 0;
    for (uint32_t i = 0; i < lib->mod.num_dynsym; i++) {
        Elf32_Sym *s = &lib->mod.dynsym[i];
        if (s->st_shndx == SHN_UNDEF || s->st_value == 0) continue;
        uintptr_t v = (lib->mod.load_addr + s->st_value) & ~1u;
        if (v <= a && v > best && (s->st_size == 0 || a < v + s->st_size)) {
            best = v;
            info->dli_sname = lib->mod.dynstr + s->st_name;
            info->dli_saddr = (void *)(lib->mod.load_addr + s->st_value);
        }
    }
    return 1;
}

/*
 * Unwinding support
 */
uintptr_t na_dl_unwind_find_exidx(uintptr_t pc, int *pcount) {
    for (int i = 0; i < NA_LIB_COUNT; i++) {
        NaLib *lib = &na_libs[i];
        if (!lib->loaded) continue;
        if (pc >= lib->mod.text_base && pc < lib->mod.text_base + lib->mod.text_size) {
            if (pcount) *pcount = (int)(lib->mod.exidx_size / 8);
            return lib->mod.exidx_base;
        }
    }
    if (pcount) *pcount = 0;
    return 0;
}

int na_dl_iterate_phdr(int (*cb)(na_dl_phdr_info *info, size_t size, void *data), void *data) {
    static char names[NA_LIB_COUNT][128];
    int r = 0;
    for (int i = 0; i < NA_LIB_COUNT && r == 0; i++) {
        NaLib *lib = &na_libs[i];
        if (!lib->loaded) continue;
        if (!names[i][0])
            snprintf(names[i], sizeof(names[i]), ANDROID_LIB_DIR "%s", lib->soname);
        na_dl_phdr_info info;
        memset(&info, 0, sizeof(info));
        info.dlpi_addr = lib->mod.load_addr;
        info.dlpi_name = names[i];
        info.dlpi_phdr = lib->phdr;
        info.dlpi_phnum = lib->phnum;
        info.dlpi_adds = NA_LIB_COUNT;
        r = cb(&info, sizeof(info), data);
    }
    return r;
}
