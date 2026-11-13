/*
 * Copyright (C) 2021      Andy Nguyen
 * Copyright (C) 2021      Rinnegatamante
 * Copyright (C) 2022-2024 Volodymyr Atamanenko
 * Copyright (C) 2026      Ellie J Turner
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

/**
 * @file  dynlib.c
 * @brief Resolving dynamic imports of the .so.
 */

#include <string.h>
#include <vitaGL.h>
#include "utils/logger.h"
#include <psp2/kernel/clib.h>
#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>
#include <inttypes.h>
#include <malloc.h>
#include <math.h>
#include <netdb.h>
#include <string.h>
#include <wchar.h>
#include <wctype.h>
#include <zlib.h>
#include <locale.h>
#include <poll.h>

#include <sys/stat.h>
#include <sys/unistd.h>
#include <sys/socket.h>
#include <sys/time.h>

#include <so_util/so_util.h>
#include <utime.h>

#include "utils/glutil.h"
#include "utils/utils.h"
#include "utils/logger.h"

#ifdef USE_SCELIBC_IO
#include <libc_bridge/libc_bridge.h>
#endif

#include <SLES/OpenSLES.h>
#include <SLES/OpenSLES_Android.h>

#include "reimpl/errno.h"
#include "reimpl/io.h"
#include "reimpl/log.h"
#include "reimpl/mem.h"
#include "reimpl/pthr.h"
#include "reimpl/sys.h"

#include "reimpl/time64.h"
#include "reimpl/asset_manager.h"

#include "na_modules.h"
#include "na_egl.h"
#include <psp2/kernel/processmgr.h>

#ifdef NDK_PORT
#include <falso_ndk/FalsoNDK.h>
#endif

const unsigned int __page_size = PAGE_SIZE;

extern void * _ZNSt9exceptionD2Ev;
extern void * _ZSt17__throw_bad_allocv;
extern void * _ZSt9terminatev;
extern void * _ZdaPv;
extern void * _ZdlPv;
extern void * _Znaj;
extern void * __cxa_allocate_exception;
extern void * __cxa_begin_catch;
extern void * __cxa_end_catch;
extern void * __cxa_free_exception;
extern void * __cxa_rethrow;
extern void * __cxa_throw;
extern void * __gxx_personality_v0;
extern void *_ZNSt8bad_castD1Ev;
extern void *_ZTISt8bad_cast;
extern void *_ZTISt9exception;
extern void *_ZTVN10__cxxabiv117__class_type_infoE;
extern void *_ZTVN10__cxxabiv120__si_class_type_infoE;
extern void *_ZTVN10__cxxabiv121__vmi_class_type_infoE;
extern void *_Znwj;
extern void *__aeabi_atexit;
extern void *__aeabi_d2lz;
extern void *__aeabi_d2ulz;
extern void *__aeabi_dadd;
extern void *__aeabi_dcmpgt;
extern void *__aeabi_dcmplt;
extern void *__aeabi_ddiv;
extern void *__aeabi_dmul;
extern void *__aeabi_f2lz;
extern void *__aeabi_f2ulz;
extern void *__aeabi_i2d;
extern void *__aeabi_idiv;
extern void *__aeabi_idivmod;
extern void *__aeabi_l2d;
extern void *__aeabi_l2f;
extern void *__aeabi_ldivmod;
extern void *__aeabi_memclr;
extern void *__aeabi_memcpy;
extern void *__aeabi_memmove;
extern void *__aeabi_memset4;
extern void *__aeabi_memset8;
extern void *__aeabi_memset;
extern void *__aeabi_ui2d;
extern void *__aeabi_uidiv;
extern void *__aeabi_uidivmod;
extern void *__aeabi_ul2d;
extern void *__aeabi_ul2f;
extern void *__aeabi_uldivmod;
extern void *__aeabi_unwind_cpp_pr0;
extern void *__aeabi_unwind_cpp_pr1;
extern void *__cxa_atexit;
extern void *__cxa_call_unexpected;
extern void *__cxa_finalize;
extern void *__cxa_guard_acquire;
extern void *__cxa_guard_release;
extern void *__cxa_pure_virtual;
extern void *__gnu_ldivmod_helper;
extern void *__gnu_unwind_frame;
extern void *__srget;
extern void *__stack_chk_guard;
extern void *__swbuf;

extern const char *BIONIC_ctype_;
extern const short *BIONIC_tolower_tab_;
extern const short *BIONIC_toupper_tab_;

static FILE __sF_fake[3];


// Bionic struct utsname: six fields of 65 chars each
struct bionic_utsname {
    char sysname[65];
    char nodename[65];
    char release[65];
    char version[65];
    char machine[65];
    char domainname[65];
};

static int uname_soloader(struct bionic_utsname *buf) {
    memset(buf, 0, sizeof(*buf));
    strcpy(buf->sysname, "Linux");
    strcpy(buf->nodename, "localhost");
    strcpy(buf->release, "3.4.0");
    strcpy(buf->version, "#1 SMP PREEMPT");
    strcpy(buf->machine, "armv7l");
    return 0;
}

SLresult ng_slCreateEngine(SLObjectItf *engine);

static SLresult slCreateEngine_ng(SLObjectItf *engine, SLuint32 numOptions, const SLEngineOption *options,
                                  SLuint32 numInterfaces, const SLInterfaceID *interfaceIds,
                                  const SLboolean *interfaceRequired) {
    // Sound is on by default. If a run with sound didn't get to the game
    // (sound_probe.txt is removed when the render loop starts, see main.c), the OpenSL ES
    // setup probably hung or crashed: start without sound from then on.
    // nosound.txt turns it off, deleting sound_probe.txt retries.
    if (file_exists(DATA_PATH "nosound.txt") || file_exists(DATA_PATH "sound_probe.txt")) {
        l_info("slCreateEngine: sound disabled (%s)", file_exists(DATA_PATH "nosound.txt") ?
               "nosound.txt" : "the last start with sound didn't finish, delete sound_probe.txt to retry");
        if (engine) *engine = NULL;
        return SL_RESULT_FEATURE_UNSUPPORTED;
    }
    FILE *probe = fopen(DATA_PATH "sound_probe.txt", "w");
    if (probe) { fputs("OpenSL ES setup started\n", probe); fclose(probe); }
    // Our own OpenSL ES (na_sles.c): the library's one hangs when the game
    // destroys a player that never played.
    SLresult r = ng_slCreateEngine(engine);
    l_info("slCreateEngine: %u", (unsigned) r);
    return r;
}

static int isfinitef_soloader(float f) {
    return isfinite(f);
}

static void _exit_soloader(int status) {
    sceKernelExitProcess(status);
}


so_default_dynlib default_dynlib[] = {
        { "_exit", (uintptr_t)&_exit_soloader },
        { "sigprocmask", (uintptr_t)&ret0 },
        { "uname", (uintptr_t)&uname_soloader },
        { "__isfinitef", (uintptr_t)&isfinitef_soloader },
        { "eglCreatePbufferSurface", (uintptr_t)&na_eglCreatePbufferSurface },
        { "eglSwapInterval", (uintptr_t)&na_eglSwapInterval },
        { "SL_IID_PLAYBACKRATE", (uintptr_t)&SL_IID_PLAYBACKRATE },
        // Common C/C++ internals
        { "_ZNSt8bad_castD1Ev", (uintptr_t)&_ZNSt8bad_castD1Ev },
        { "_ZNSt9exceptionD2Ev", (uintptr_t)&_ZNSt9exceptionD2Ev },
        { "_ZSt17__throw_bad_allocv", (uintptr_t)&_ZSt17__throw_bad_allocv },
        { "_ZSt9terminatev", (uintptr_t)&_ZSt9terminatev },
        { "_ZTISt8bad_cast", (uintptr_t)&_ZTISt8bad_cast },
        { "_ZTISt9exception", (uintptr_t)&_ZTISt9exception },
        { "_ZTVN10__cxxabiv117__class_type_infoE", (uintptr_t)&_ZTVN10__cxxabiv117__class_type_infoE },
        { "_ZTVN10__cxxabiv120__si_class_type_infoE", (uintptr_t)&_ZTVN10__cxxabiv120__si_class_type_infoE },
        { "_ZTVN10__cxxabiv121__vmi_class_type_infoE", (uintptr_t)&_ZTVN10__cxxabiv121__vmi_class_type_infoE },
        { "_ZdaPv", (uintptr_t)&_ZdaPv },
        { "_ZdlPv", (uintptr_t)&_ZdlPv },
        { "_Znaj", (uintptr_t)&_Znaj },
        { "_Znwj", (uintptr_t)&_Znwj },
        { "__aeabi_atexit", (uintptr_t)&__aeabi_atexit },
        { "__aeabi_d2lz", (uintptr_t)&__aeabi_d2lz },
        { "__aeabi_d2ulz", (uintptr_t)&__aeabi_d2ulz },
        { "__aeabi_dadd", (uintptr_t)&__aeabi_dadd },
        { "__aeabi_dcmpgt", (uintptr_t)&__aeabi_dcmpgt },
        { "__aeabi_dcmplt", (uintptr_t)&__aeabi_dcmplt },
        { "__aeabi_ddiv", (uintptr_t)&__aeabi_ddiv },
        { "__aeabi_dmul", (uintptr_t)&__aeabi_dmul },
        { "__aeabi_f2lz", (uintptr_t)&__aeabi_f2lz },
        { "__aeabi_f2ulz", (uintptr_t)&__aeabi_f2ulz },
        { "__aeabi_i2d", (uintptr_t)&__aeabi_i2d },
        { "__aeabi_idiv", (uintptr_t)&__aeabi_idiv },
        { "__aeabi_idivmod", (uintptr_t)&__aeabi_idivmod },
        { "__aeabi_l2d", (uintptr_t)&__aeabi_l2d },
        { "__aeabi_l2f", (uintptr_t)&__aeabi_l2f },
        { "__aeabi_ldivmod", (uintptr_t)&__aeabi_ldivmod },
        { "__aeabi_memclr", (uintptr_t)&__aeabi_memclr },
        { "__aeabi_memclr4", (uintptr_t)&__aeabi_memclr },
        { "__aeabi_memclr8", (uintptr_t)&__aeabi_memclr },
        { "__aeabi_memcpy", (uintptr_t)&sceClibMemcpy },
        { "__aeabi_memcpy4", (uintptr_t)&sceClibMemcpy },
        { "__aeabi_memcpy8", (uintptr_t)&sceClibMemcpy },
        { "__aeabi_memmove", (uintptr_t)&__aeabi_memmove },
        { "__aeabi_memmove4", (uintptr_t)&__aeabi_memmove },
        { "__aeabi_memmove8", (uintptr_t)&__aeabi_memmove },
        { "__aeabi_memset", (uintptr_t)&__aeabi_memset },
        { "__aeabi_memset4",  (uintptr_t)&__aeabi_memset4 },
        { "__aeabi_memset8", (uintptr_t)&__aeabi_memset8 },
        { "__aeabi_ui2d", (uintptr_t)&__aeabi_ui2d },
        { "__aeabi_uidiv", (uintptr_t)&__aeabi_uidiv },
        { "__aeabi_uidivmod", (uintptr_t)&__aeabi_uidivmod },
        { "__aeabi_ul2d", (uintptr_t)&__aeabi_ul2d },
        { "__aeabi_ul2f", (uintptr_t)&__aeabi_ul2f },
        { "__aeabi_uldivmod", (uintptr_t)&__aeabi_uldivmod },
        { "__aeabi_unwind_cpp_pr0", (uintptr_t)&__aeabi_unwind_cpp_pr0 },
        { "__aeabi_unwind_cpp_pr1", (uintptr_t)&__aeabi_unwind_cpp_pr1 },
        { "__atomic_cmpxchg", (uintptr_t)&__atomic_cmpxchg },
        { "__atomic_dec", (uintptr_t)&__atomic_dec },
        { "__atomic_inc", (uintptr_t)&__atomic_inc },
        { "__atomic_swap", (uintptr_t)&__atomic_swap },
        { "__cxa_allocate_exception", (uintptr_t)&__cxa_allocate_exception },
        { "__cxa_atexit", (uintptr_t)&__cxa_atexit },
        { "__cxa_begin_catch", (uintptr_t)&__cxa_begin_catch },
        { "__cxa_begin_cleanup", (uintptr_t)&ret0 },
        { "__cxa_call_unexpected", (uintptr_t)&__cxa_call_unexpected },
        { "__cxa_end_catch", (uintptr_t)&__cxa_end_catch },
        { "__cxa_finalize", (uintptr_t)&__cxa_finalize },
        { "__cxa_free_exception", (uintptr_t)&__cxa_free_exception },
        { "__cxa_guard_acquire", (uintptr_t)&__cxa_guard_acquire },
        { "__cxa_guard_release", (uintptr_t)&__cxa_guard_release },
        { "__cxa_pure_virtual", (uintptr_t)&__cxa_pure_virtual },
        { "__cxa_rethrow", (uintptr_t)&__cxa_rethrow },
        { "__cxa_throw", (uintptr_t)&__cxa_throw },
        { "__cxa_type_match", (uintptr_t)&ret0 },
        { "__gnu_Unwind_Find_exidx", (uintptr_t)&na_dl_unwind_find_exidx },
        { "__gnu_ldivmod_helper", (uintptr_t)&__gnu_ldivmod_helper },
        { "__gnu_unwind_frame", (uintptr_t)&__gnu_unwind_frame },
        { "__google_potentially_blocking_region_begin", (uintptr_t)&ret0 },
        { "__google_potentially_blocking_region_end", (uintptr_t)&ret0 },
        { "__gxx_personality_v0", (uintptr_t)&__gxx_personality_v0 },
        { "__isinf", (uintptr_t)&ret0 },
        { "__page_size", (uintptr_t)&__page_size },
        { "__sF", (uintptr_t)&__sF_fake },
        { "__srget", (uintptr_t)&__srget },
        { "__stack_chk_fail", (uintptr_t)&__stack_chk_fail_soloader },
        { "__stack_chk_guard", (uintptr_t)&__stack_chk_guard },
        { "__swbuf", (uintptr_t)&__swbuf },
        { "__system_property_get", (uintptr_t)&__system_property_get_soloader },
        { "__assert2", (uintptr_t)&ret0 }, // TODO: stub/impl
        { "dl_unwind_find_exidx", (uintptr_t)&na_dl_unwind_find_exidx },


        // ctype
        { "_ctype_", (uintptr_t)&BIONIC_ctype_ },
        { "_tolower_tab_", (uintptr_t)&BIONIC_tolower_tab_ },
        { "_toupper_tab_", (uintptr_t)&BIONIC_toupper_tab_ },
        { "isalnum", (uintptr_t)&isalnum },
        { "isalpha", (uintptr_t)&isalpha },
        { "isblank", (uintptr_t)&isblank },
        { "iscntrl", (uintptr_t)&iscntrl },
        { "isgraph", (uintptr_t)&isgraph },
        { "islower", (uintptr_t)&islower },
        { "isprint", (uintptr_t)&isprint },
        { "ispunct", (uintptr_t)&ispunct },
        { "isspace", (uintptr_t)&isspace },
        { "isupper", (uintptr_t)&isupper },
        { "isxdigit", (uintptr_t)&isxdigit },
        { "tolower", (uintptr_t)&tolower },
        { "toupper", (uintptr_t)&toupper },


        // Android SDK standard logging
        { "__android_log_assert", (uintptr_t)&__android_log_assert },
        { "__android_log_print", (uintptr_t)&__android_log_print },
        { "__android_log_vprint", (uintptr_t)&__android_log_vprint },
        { "__android_log_write", (uintptr_t)&__android_log_write },
        { "__android_log_buf_write", (uintptr_t)&__android_log_buf_write },


        // AAssetManager
        { "AAsset_close", (uintptr_t)&AAsset_close },
        { "AAsset_getLength", (uintptr_t)&AAsset_getLength },
        { "AAsset_getBuffer", (uintptr_t)&AAsset_getBuffer },
        { "AAsset_getRemainingLength", (uintptr_t)&AAsset_getRemainingLength },
        { "AAsset_read", (uintptr_t)&AAsset_read },
        { "AAsset_seek", (uintptr_t)&AAsset_seek },
        { "AAsset_openFileDescriptor", (uintptr_t)&AAsset_openFileDescriptor },
        { "AAssetDir_close", (uintptr_t)&AAssetDir_close },
        { "AAssetDir_getNextFileName", (uintptr_t)&AAssetDir_getNextFileName },
        { "AAssetManager_fromJava", (uintptr_t)&ret1 },
        { "AAssetManager_open", (uintptr_t)&AAssetManager_open },
        { "AAssetManager_openDir", (uintptr_t)&AAssetManager_openDir },

        
#ifdef NDK_PORT
        // FalsoNDK
        {"AConfiguration_delete", (uintptr_t)&AConfiguration_delete},
        {"AConfiguration_fromAssetManager", (uintptr_t)&AConfiguration_fromAssetManager},
        {"AConfiguration_getCountry", (uintptr_t)&AConfiguration_getCountry},
        {"AConfiguration_getLanguage", (uintptr_t)&AConfiguration_getLanguage},
        {"AConfiguration_new", (uintptr_t)&AConfiguration_new},
        {"AInputEvent_getDeviceId", (uintptr_t)&AInputEvent_getDeviceId},
        {"AInputEvent_getSource", (uintptr_t)&AInputEvent_getSource},
        {"AInputEvent_getType", (uintptr_t)&AInputEvent_getType},
        {"AInputQueue_attachLooper", (uintptr_t)&AInputQueue_attachLooper},
        {"AInputQueue_detachLooper", (uintptr_t)&AInputQueue_detachLooper},
        {"AInputQueue_finishEvent", (uintptr_t)&AInputQueue_finishEvent},
        {"AInputQueue_getEvent", (uintptr_t)&AInputQueue_getEvent},
        {"AInputQueue_hasEvents", (uintptr_t)&AInputQueue_hasEvents},
        {"AInputQueue_preDispatchEvent", (uintptr_t)&AInputQueue_preDispatchEvent},
        {"AKeyEvent_getAction", (uintptr_t)&AKeyEvent_getAction},
        {"AKeyEvent_getKeyCode", (uintptr_t)&AKeyEvent_getKeyCode},
        {"ALooper_addFd", (uintptr_t)&ALooper_addFd},
        {"ALooper_pollAll", (uintptr_t)&ALooper_pollAll},
        {"ALooper_prepare", (uintptr_t)&ALooper_prepare},
        {"AMotionEvent_getAction", (uintptr_t)&AMotionEvent_getAction},
        {"AMotionEvent_getAxisValue", (uintptr_t)&AMotionEvent_getAxisValue},
        {"AMotionEvent_getPointerCount", (uintptr_t)&AMotionEvent_getPointerCount},
        {"AMotionEvent_getX", (uintptr_t)&AMotionEvent_getX},
        {"AMotionEvent_getY", (uintptr_t)&AMotionEvent_getY},
        {"ANativeActivity_finish", (uintptr_t)&ANativeActivity_finish},
        {"ANativeActivity_setWindowFlags", (uintptr_t)&ANativeActivity_setWindowFlags},
        {"ANativeWindow_getHeight", (uintptr_t)&ANativeWindow_getHeight},
        {"ANativeWindow_getWidth", (uintptr_t)&ANativeWindow_getWidth},
        {"ANativeWindow_setBuffersGeometry", (uintptr_t)&ANativeWindow_setBuffersGeometry},
#endif


        // Math
        { "acos", (uintptr_t)&acos },
        { "acosf", (uintptr_t)&acosf },
        { "arc4random", (uintptr_t)&arc4random },
        { "asin", (uintptr_t)&asin },
        { "asinf", (uintptr_t)&asinf },
        { "atan", (uintptr_t)&atan },
        { "atan2", (uintptr_t)&atan2 },
        { "atan2f", (uintptr_t)&atan2f },
        { "atanf", (uintptr_t)&atanf },
        { "ceil", (uintptr_t)&ceil },
        { "ceilf", (uintptr_t)&ceilf },
        { "cos", (uintptr_t)&cos },
        { "cosf", (uintptr_t)&cosf },
        { "cosh", (uintptr_t)&cosh },
        { "exp", (uintptr_t)&exp },
        { "exp2", (uintptr_t)&exp2 },
        { "exp2f", (uintptr_t)&exp2f },
        { "expf", (uintptr_t)&expf },
        { "floor", (uintptr_t)&floor },
        { "floorf", (uintptr_t)&floorf },
        { "fmax", (uintptr_t)&fmax },
        { "fmaxf", (uintptr_t)&fmaxf },
        { "fmin", (uintptr_t)&fmin },
        { "fminf", (uintptr_t)&fminf },
        { "frexpf", (uintptr_t)&frexpf },
        { "fmod", (uintptr_t)&fmod },
        { "fmodf", (uintptr_t)&fmodf },
        { "frexp", (uintptr_t)&frexp },
        { "ldexp", (uintptr_t)&ldexp },
        { "ldexpf", (uintptr_t)&ldexpf },
        { "log", (uintptr_t)&log },
        { "log10", (uintptr_t)&log10 },
        { "log10f", (uintptr_t)&log10f },
        { "logf", (uintptr_t)&logf },
        { "lrint", (uintptr_t)&lrint },
        { "lrintf", (uintptr_t)&lrintf },
        { "lround", (uintptr_t)&lround },
        { "lroundf", (uintptr_t)&lroundf },
        { "modf", (uintptr_t)&modf },
        { "pow", (uintptr_t)&pow },
        { "powf", (uintptr_t)&powf },
        { "rint", (uintptr_t)&rint },
        { "rintf", (uintptr_t)&rintf },
        { "round", (uintptr_t)&round },
        { "roundf", (uintptr_t)&roundf },
        { "scalbn", (uintptr_t)&scalbn },
        { "scalbnf", (uintptr_t)&scalbnf },
        { "sin", (uintptr_t)&sin },
        { "sincos", (uintptr_t)&sincos },
        { "sincosf", (uintptr_t)&sincosf },
        { "sinf", (uintptr_t)&sinf },
        { "sinh", (uintptr_t)&sinh },
        { "sinhf", (uintptr_t)&sinhf },
        { "sqrt", (uintptr_t)&sqrt },
        { "sqrtf", (uintptr_t)&sqrtf },
        { "tan", (uintptr_t)&tan },
        { "tanf", (uintptr_t)&tanf },
        { "tanh", (uintptr_t)&tanh },
        { "trunc", (uintptr_t)&trunc },
        { "truncf", (uintptr_t)&truncf },


        // Sockets
        { "accept", (uintptr_t)&accept },
        { "bind", (uintptr_t)&bind },
        { "connect", (uintptr_t)&connect },
        { "freeaddrinfo", (uintptr_t)&freeaddrinfo },
        { "gai_strerror", (uintptr_t)&ret0 },
        { "getaddrinfo", (uintptr_t)&getaddrinfo },
        { "gethostbyaddr", (uintptr_t)&gethostbyaddr },
        { "gethostbyname", (uintptr_t)&gethostbyname },
        { "gethostname", (uintptr_t)&gethostname },
        { "getpeername", (uintptr_t)&getpeername },
        { "getservbyname", (uintptr_t)&getservbyname },
        { "getsockname", (uintptr_t)&getsockname },
        { "getsockopt", (uintptr_t)&getsockopt },
        { "inet_aton", (uintptr_t)&inet_aton },
        { "inet_pton", (uintptr_t)&inet_pton },
        { "inet_ntoa", (uintptr_t)&inet_ntoa },
        { "inet_ntop", (uintptr_t)&inet_ntop },
        { "listen", (uintptr_t)&listen },
        { "poll", (uintptr_t)&poll },
        { "recv", (uintptr_t)&recv },
        { "recvfrom", (uintptr_t)&recvfrom },
        { "recvmsg", (uintptr_t)&recvmsg },
        { "select", (uintptr_t)&select },
        { "send", (uintptr_t)&send },
        { "sendmsg", (uintptr_t)&sendmsg },
        { "sendto", (uintptr_t)&sendto },
        { "setsockopt", (uintptr_t)&setsockopt },
        { "shutdown", (uintptr_t)&shutdown },
        { "socket", (uintptr_t)&socket },


        // Memory
        { "calloc", (uintptr_t)&calloc },
        { "free", (uintptr_t)&free },
        { "malloc", (uintptr_t)&malloc },
        { "memalign", (uintptr_t)&memalign },
        { "memcmp", (uintptr_t)&memcmp },
        { "memcpy", (uintptr_t)&sceClibMemcpy },
        { "memmem", (uintptr_t)&memmem },
        { "memmove", (uintptr_t)&memmove },
        { "memset", (uintptr_t)&memset },
        { "mmap", (uintptr_t)&mmap },
        { "__mmap2", (uintptr_t)&mmap },
        { "munmap", (uintptr_t)&munmap },
        { "realloc", (uintptr_t)&realloc },
        { "valloc", (uintptr_t)&valloc },


        // IO
#ifndef NDK_PORT
        { "close", (uintptr_t)&close_soloader },
#else   
        { "close", (uintptr_t)&fndk_close },
#endif
        { "closedir", (uintptr_t)&closedir_soloader },
        { "execv", (uintptr_t)&ret0 },
        { "fclose", (uintptr_t)&fclose_soloader },
        { "fcntl", (uintptr_t)&fcntl_soloader },
        { "fopen", (uintptr_t)&fopen_soloader },
        { "fstat", (uintptr_t)&fstat_soloader },
        { "fsync", (uintptr_t)&fsync_soloader },
        { "ioctl", (uintptr_t)&ioctl_soloader },
        { "__open_2", (uintptr_t)&open_soloader },
        { "open", (uintptr_t)&open_soloader },
        { "opendir", (uintptr_t)&opendir_soloader },
        { "readdir", (uintptr_t)&readdir_soloader },
        { "readdir_r", (uintptr_t)&readdir_r_soloader },
        { "stat", (uintptr_t)&stat_soloader },
        { "utime", (uintptr_t)&utime },

        #ifdef USE_SCELIBC_IO
            { "fdopen", (uintptr_t)&sceLibcBridge_fdopen },
            { "feof", (uintptr_t)&sceLibcBridge_feof },
            { "ferror", (uintptr_t)&sceLibcBridge_ferror },
            { "fflush", (uintptr_t)&sceLibcBridge_fflush },
            { "fgetc", (uintptr_t)&sceLibcBridge_fgetc },
            { "fgetpos", (uintptr_t)&sceLibcBridge_fgetpos },
            { "fgets", (uintptr_t)&sceLibcBridge_fgets },
            { "fileno", (uintptr_t)&sceLibcBridge_fileno },
            { "fputc", (uintptr_t)&sceLibcBridge_fputc },
            { "fputs", (uintptr_t)&sceLibcBridge_fputs },
            { "fread", (uintptr_t)&sceLibcBridge_fread },
            { "freopen", (uintptr_t)&sceLibcBridge_freopen },
            { "fseek", (uintptr_t)&sceLibcBridge_fseek },
            { "fsetpos", (uintptr_t)&sceLibcBridge_fsetpos },
            { "ftell", (uintptr_t)&sceLibcBridge_ftell },
            { "fwide", (uintptr_t)&sceLibcBridge_fwide },
            { "fwrite", (uintptr_t)&sceLibcBridge_fwrite },
            { "getc", (uintptr_t)&sceLibcBridge_getc },
            { "getwc", (uintptr_t)&sceLibcBridge_getwc },
            { "putc", (uintptr_t)&sceLibcBridge_putc },
            { "putchar", (uintptr_t)&sceLibcBridge_putchar },
            { "puts", (uintptr_t)&sceLibcBridge_puts },
            { "putwc", (uintptr_t)&sceLibcBridge_putwc },
            { "setvbuf", (uintptr_t)&sceLibcBridge_setvbuf },
            { "ungetc", (uintptr_t)&sceLibcBridge_ungetc },
            { "ungetwc", (uintptr_t)&sceLibcBridge_ungetwc },
        #else
            { "fdopen", (uintptr_t)&fdopen },
            { "feof", (uintptr_t)&feof },
            { "ferror", (uintptr_t)&ferror },
            { "fflush", (uintptr_t)&fflush },
            { "fgetc", (uintptr_t)&fgetc },
            { "fgetpos", (uintptr_t)&fgetpos },
            { "fgets", (uintptr_t)&fgets },
            { "fileno", (uintptr_t)&fileno },
            { "fputc", (uintptr_t)&fputc },
            { "fputs", (uintptr_t)&fputs },
            { "fread", (uintptr_t)&fread },
            { "freopen", (uintptr_t)&freopen },
            { "fseek", (uintptr_t)&fseek },
            { "fsetpos", (uintptr_t)&fsetpos },
            { "ftell", (uintptr_t)&ftell },
            { "fwide", (uintptr_t)&fwide },
            { "fwrite", (uintptr_t)&fwrite },
            { "getc", (uintptr_t)&getc },
            { "getwc", (uintptr_t)&getwc },
            { "putc", (uintptr_t)&putc },
            { "putchar", (uintptr_t)&putchar },
            { "puts", (uintptr_t)&puts },
            { "putwc", (uintptr_t)&putwc },
            { "setvbuf", (uintptr_t)&setvbuf },
            { "ungetc", (uintptr_t)&ungetc },
            { "ungetwc", (uintptr_t)&ungetwc },
        #endif

        { "access", (uintptr_t)&access },
        { "basename", (uintptr_t)&basename },
        { "chdir", (uintptr_t)&chdir },
        { "chmod", (uintptr_t)&chmod },
        { "dup", (uintptr_t)&dup },
        { "fseeko", (uintptr_t)&fseeko }, // TODO: wrap normal fseek for SceLibc version?
        { "ftello", (uintptr_t)&ftello },
        { "ftruncate", (uintptr_t)&ftruncate },
        { "getcwd", (uintptr_t)&getcwd },
        { "lseek", (uintptr_t)&lseek },
        { "lseek64", (uintptr_t)&ret0 }, // TODO: implement or stub with warning
        { "lstat", (uintptr_t)&lstat },
        { "mkdir", (uintptr_t)&mkdir },
#ifndef NDK_PORT
        { "pipe", (uintptr_t)&pipe },
        { "read", (uintptr_t)&read },
#else
        { "pipe", (uintptr_t)&fndk_pipe },
        { "read", (uintptr_t)&fndk_read },
#endif
        { "realpath", (uintptr_t)&realpath },
        { "remove", (uintptr_t)&remove },
        { "rename", (uintptr_t)&rename },
        { "rewind", (uintptr_t)&rewind },
        { "rmdir", (uintptr_t)&rmdir },
        { "truncate", (uintptr_t)&truncate },
        { "unlink", (uintptr_t)&unlink },
#ifndef NDK_PORT
        { "write", (uintptr_t)&write },
#else
        { "write", (uintptr_t)&fndk_write },
#endif


        // *printf, *scanf
        { "snprintf", (uintptr_t)&snprintf },
        { "sprintf", (uintptr_t)&sprintf },
        { "vasprintf", (uintptr_t)&vasprintf },
        { "vprintf", (uintptr_t)&vprintf },
        { "vsnprintf", (uintptr_t)&vsnprintf },
        { "vsprintf", (uintptr_t)&vsprintf },
        { "vsscanf", (uintptr_t)&vsscanf },
        { "vswprintf", (uintptr_t)&vswprintf },
        { "printf", (uintptr_t)&sceClibPrintf },
        { "swprintf", (uintptr_t)&swprintf },

        #ifdef USE_SCELIBC_IO
            { "fprintf", (uintptr_t)&sceLibcBridge_fprintf },
            { "fscanf", (uintptr_t)&sceLibcBridge_fscanf },
            { "sscanf", (uintptr_t)&sceLibcBridge_sscanf },
            { "vfprintf", (uintptr_t)&sceLibcBridge_vfprintf },
        #else
            { "fprintf", (uintptr_t)&fprintf },
            { "fscanf", (uintptr_t)&fscanf },
            { "sscanf", (uintptr_t)&sscanf },
            { "vfprintf", (uintptr_t)&vfprintf },
        #endif


        // EGL
        { "eglBindAPI", (uintptr_t)&na_eglBindAPI },
        { "eglChooseConfig", (uintptr_t)&na_eglChooseConfig },
        { "eglCreateContext", (uintptr_t)&na_eglCreateContext },
        { "eglCreateWindowSurface", (uintptr_t)&na_eglCreateWindowSurface },
        { "eglDestroyContext", (uintptr_t)&na_eglDestroyContext },
        { "eglDestroySurface", (uintptr_t)&na_eglDestroySurface },
        { "eglGetConfigAttrib", (uintptr_t)&na_eglGetConfigAttrib },
        { "eglGetConfigs", (uintptr_t)&na_eglGetConfigs },
        { "eglGetCurrentContext", (uintptr_t)&na_eglGetCurrentContext },
        { "eglGetDisplay", (uintptr_t)&na_eglGetDisplay },
        { "eglGetError", (uintptr_t)&na_eglGetError },
        { "eglGetProcAddress", (uintptr_t)&na_eglGetProcAddress },
        { "eglInitialize", (uintptr_t)&na_eglInitialize },
        { "eglMakeCurrent", (uintptr_t)&na_eglMakeCurrent },
        { "eglQueryContext", (uintptr_t)&na_eglQueryContext },
        { "eglQueryString", (uintptr_t)&na_eglQueryString },
        { "eglQuerySurface", (uintptr_t)&na_eglQuerySurface },
        { "eglSwapBuffers", (uintptr_t)&na_eglSwapBuffers },
        { "eglGetCurrentDisplay", (uintptr_t)&na_eglGetCurrentDisplay },
        { "eglGetCurrentSurface", (uintptr_t)&na_eglGetCurrentSurface },
        { "eglTerminate", (uintptr_t)&na_eglTerminate },


        // OpenGL


        // OpenSLES
        { "SL_IID_ENGINE", (uintptr_t)&SL_IID_ENGINE },
        { "SL_IID_ANDROIDSIMPLEBUFFERQUEUE", (uintptr_t)&SL_IID_ANDROIDSIMPLEBUFFERQUEUE },
        { "SL_IID_BUFFERQUEUE", (uintptr_t)&SL_IID_BUFFERQUEUE },
        { "SL_IID_METADATAEXTRACTION", (uintptr_t)&SL_IID_METADATAEXTRACTION },
        { "SL_IID_PITCH", (uintptr_t)&SL_IID_PITCH },
        { "SL_IID_PLAY", (uintptr_t)&SL_IID_PLAY },
        { "SL_IID_PREFETCHSTATUS", (uintptr_t)&SL_IID_PREFETCHSTATUS },
        { "SL_IID_SEEK", (uintptr_t)&SL_IID_SEEK },
        { "SL_IID_VOLUME", (uintptr_t)&SL_IID_VOLUME },
        { "slCreateEngine", (uintptr_t)&slCreateEngine_ng },


        // Pthread
        { "pthread_attr_destroy", (uintptr_t)&pthread_attr_destroy_soloader },
        { "pthread_attr_init", (uintptr_t) &pthread_attr_init_soloader },
        { "pthread_attr_setdetachstate", (uintptr_t) &pthread_attr_setdetachstate_soloader },
        { "pthread_attr_setstacksize", (uintptr_t) &pthread_attr_setstacksize_soloader },
        { "pthread_attr_setschedparam", (uintptr_t) &ret0 },

        { "pthread_cond_broadcast", (uintptr_t) &pthread_cond_broadcast_soloader },
        { "pthread_cond_destroy", (uintptr_t) &pthread_cond_destroy_soloader },
        { "pthread_cond_init", (uintptr_t) &pthread_cond_init_soloader },
        { "pthread_cond_signal", (uintptr_t) &pthread_cond_signal_soloader },
        { "pthread_cond_timedwait", (uintptr_t) &pthread_cond_timedwait_soloader },
        { "pthread_cond_wait", (uintptr_t) &pthread_cond_wait_soloader },

        { "pthread_create", (uintptr_t) &pthread_create_soloader },
        { "pthread_detach", (uintptr_t) &pthread_detach_soloader },
        { "pthread_equal", (uintptr_t) &pthread_equal_soloader },
        { "pthread_exit", (uintptr_t)&pthread_exit },
        { "pthread_getschedparam", (uintptr_t) &pthread_getschedparam_soloader },
        { "pthread_getspecific", (uintptr_t)&pthread_getspecific },
        { "pthread_join", (uintptr_t) &pthread_join_soloader },
        { "pthread_key_create", (uintptr_t)&pthread_key_create },
        { "pthread_key_delete", (uintptr_t)&pthread_key_delete },
        { "pthread_kill", (uintptr_t)&pthread_kill_soloader },

        { "pthread_mutex_destroy", (uintptr_t) &pthread_mutex_destroy_soloader },
        { "pthread_mutex_init", (uintptr_t) &pthread_mutex_init_soloader },
        { "pthread_mutex_lock", (uintptr_t) &pthread_mutex_lock_soloader },
        { "pthread_mutex_trylock", (uintptr_t) &pthread_mutex_trylock_soloader },
        { "pthread_mutex_unlock", (uintptr_t) &pthread_mutex_unlock_soloader },
        { "pthread_mutexattr_destroy", (uintptr_t) &pthread_mutexattr_destroy_soloader },
        { "pthread_mutexattr_init", (uintptr_t) &pthread_mutexattr_init_soloader },
        { "pthread_mutexattr_settype", (uintptr_t) &pthread_mutexattr_settype_soloader },
        { "pthread_mutexattr_setpshared", (uintptr_t) &ret0 },
        { "pthread_once", (uintptr_t)&pthread_once_soloader },

        { "pthread_self", (uintptr_t) &pthread_self_soloader },
        { "pthread_setname_np", (uintptr_t) &pthread_setname_np_soloader },
        { "pthread_setschedparam", (uintptr_t) &pthread_setschedparam_soloader },
        { "pthread_setspecific", (uintptr_t)&pthread_setspecific },
        { "pthread_sigmask", (uintptr_t)&ret0 },

        { "sem_destroy", (uintptr_t) &sem_destroy_soloader },
        { "sem_getvalue", (uintptr_t) &sem_getvalue_soloader },
        { "sem_init", (uintptr_t) &sem_init_soloader },
        { "sem_post", (uintptr_t) &sem_post_soloader },
        { "sem_timedwait", (uintptr_t) &sem_timedwait_soloader },
        { "sem_trywait", (uintptr_t) &sem_trywait_soloader },
        { "sem_wait", (uintptr_t) &sem_wait_soloader },

        { "sched_get_priority_max", (uintptr_t)&sched_get_priority_max },
        { "sched_get_priority_min", (uintptr_t)&sched_get_priority_min },
        { "sched_yield", (uintptr_t)&sched_yield },


        // wchar, wctype
        { "btowc", (uintptr_t)&btowc },
        { "iswalpha", (uintptr_t)&iswalpha },
        { "iswcntrl", (uintptr_t)&iswcntrl },
        { "iswctype", (uintptr_t)&iswctype },
        { "iswdigit", (uintptr_t)&iswdigit },
        { "iswdigit", (uintptr_t)&iswdigit },
        { "iswlower", (uintptr_t)&iswlower },
        { "iswprint", (uintptr_t)&iswprint },
        { "iswpunct", (uintptr_t)&iswpunct },
        { "iswspace", (uintptr_t)&iswspace },
        { "iswupper", (uintptr_t)&iswupper },
        { "iswxdigit", (uintptr_t)&iswxdigit },
        { "mbrlen", (uintptr_t)&mbrlen },
        { "mbrtowc", (uintptr_t)&mbrtowc },
        { "mbsnrtowcs", (uintptr_t)&mbsnrtowcs },
        { "mbsrtowcs", (uintptr_t)&mbsrtowcs },
        { "mbstowcs", (uintptr_t)&mbstowcs },
        { "mbtowc", (uintptr_t)&mbtowc },
        { "towlower", (uintptr_t)&towlower },
        { "towupper", (uintptr_t)&towupper },
        { "wcrtomb", (uintptr_t)&wcrtomb },
        { "wcscasecmp", (uintptr_t)&wcscasecmp },
        { "wcscmp", (uintptr_t)&wcscmp },
        { "wcscoll", (uintptr_t)&wcscoll },
        { "wcscpy", (uintptr_t)&wcscpy },
        { "wcsftime", (uintptr_t)&wcsftime },
        { "wcslcat", (uintptr_t)&wcslcat },
        { "wcslcpy", (uintptr_t)&wcslcpy },
        { "wcslen", (uintptr_t)&wcslen },
        { "wcsncasecmp", (uintptr_t)&wcsncasecmp },
        { "wcsncmp", (uintptr_t)&wcsncmp },
        { "wcsncpy", (uintptr_t)&wcsncpy },
        { "wcsnlen", (uintptr_t)&wcsnlen },
        { "wcsnrtombs", (uintptr_t)&wcsnrtombs },
        { "wcsstr", (uintptr_t)&wcsstr },
        { "wcstod", (uintptr_t)&wcstod },
        { "wcstof", (uintptr_t)&wcstof },
        { "wcstol", (uintptr_t)&wcstol },
        { "wcstoll", (uintptr_t)&wcstoll },
        { "wcstombs", (uintptr_t)&wcstombs },
        { "wcstoul", (uintptr_t)&wcstoul },
        { "wcstoull", (uintptr_t)&wcstoull },
        { "wcsxfrm", (uintptr_t)&wcsxfrm },
        { "wctob", (uintptr_t)&wctob },
        { "wctype", (uintptr_t)&wctype },
        { "wmemchr", (uintptr_t)&wmemchr },
        { "wmemcmp", (uintptr_t)&wmemcmp },
        { "wmemcpy", (uintptr_t)&wmemcpy },
        { "wmemmove", (uintptr_t)&wmemmove },
        { "wmemset", (uintptr_t)&wmemset },


        // libdl
        { "dlclose", (uintptr_t)&na_dlclose },
        { "dlerror", (uintptr_t)&na_dlerror },
        { "dlopen", (uintptr_t)&na_dlopen },
        { "dlsym", (uintptr_t)&na_dlsym },


        // Errno
        { "__errno", (uintptr_t)&__errno_soloader },
        { "strerror", (uintptr_t)&strerror_soloader },
        { "strerror_r", (uintptr_t)&strerror_r_soloader },
        { "perror", (uintptr_t)&perror }, // TODO: errno translation


        // Strings
        { "memchr", (uintptr_t)&memchr },
        { "memrchr", (uintptr_t)&memrchr },
        { "strcasecmp", (uintptr_t)&strcasecmp },
        { "strcat", (uintptr_t)&strcat },
        { "strchr", (uintptr_t)&strchr },
        { "strcmp", (uintptr_t)&strcmp },
        { "strcoll", (uintptr_t)&strcoll },
        { "strcpy", (uintptr_t)&strcpy },
        { "strcspn", (uintptr_t)&strcspn },
        { "strdup", (uintptr_t)&strdup },
        { "strlcat", (uintptr_t)&strlcat },
        { "strlcpy", (uintptr_t)&strlcpy },
        { "strlen", (uintptr_t)&strlen },
        { "strncasecmp", (uintptr_t)&strncasecmp },
        { "strncat", (uintptr_t)&strncat },
        { "strncmp", (uintptr_t)&strncmp },
        { "strncpy", (uintptr_t)&strncpy },
        { "strnlen", (uintptr_t)&strnlen },
        { "strpbrk", (uintptr_t)&strpbrk },
        { "strrchr", (uintptr_t)&strrchr },
        { "strspn", (uintptr_t)&strspn },
        { "strstr", (uintptr_t)&strstr },
        { "strtok", (uintptr_t)&strtok },
        { "strtok_r", (uintptr_t)&strtok_r },
        { "strxfrm", (uintptr_t)&strxfrm },


        // Syscalls
        { "fork", (uintptr_t)&fork },
        { "getpagesize", (uintptr_t)&getpagesize },
        { "getpid", (uintptr_t)&getpid },
        { "sbrk", (uintptr_t)&sbrk },
        { "syscall", (uintptr_t)&syscall },
        { "sysconf", (uintptr_t)&ret0 },
        { "system", (uintptr_t)&system },
        { "waitpid", (uintptr_t)&ret0 },


        // Time
        { "clock", (uintptr_t)&clock_soloader },
        { "clock_getres", (uintptr_t)&clock_getres_soloader },
        { "clock_gettime", (uintptr_t)&clock_gettime_soloader },
        { "difftime", (uintptr_t)&difftime },
        { "gettimeofday", (uintptr_t)&gettimeofday },
        { "gmtime", (uintptr_t)&gmtime },
        { "gmtime64", (uintptr_t)&gmtime64 },
        { "gmtime_r", (uintptr_t)&gmtime_r },
        { "localtime", (uintptr_t)&localtime },
        { "localtime64", (uintptr_t)&localtime64 },
        { "localtime_r", (uintptr_t)&localtime_r },
        { "mktime", (uintptr_t)&mktime },
        { "mktime64", (uintptr_t)&mktime64 },
        { "nanosleep", (uintptr_t)&nanosleep },
        { "strftime", (uintptr_t)&strftime },
        { "time", (uintptr_t)&time },
        { "tzset", (uintptr_t)&tzset },


        // Temp
        { "mkstemp", (uintptr_t)&mkstemp },
        { "mktemp", (uintptr_t)&mktemp },
        { "tmpfile", (uintptr_t)&tmpfile },
        { "tmpnam", (uintptr_t)&tmpnam },


        // stdlib
        { "abort", (uintptr_t)&abort_soloader },
        { "atof", (uintptr_t)&atof },
        { "atoi", (uintptr_t)&atoi },
        { "atol", (uintptr_t)&atol },
        { "atoll", (uintptr_t)&atoll },
        { "bsearch", (uintptr_t)&bsearch },
        { "exit", (uintptr_t)&exit_soloader },
        { "lrand48", (uintptr_t)&lrand48 },
        { "prctl", (uintptr_t)&ret0 },
        { "sleep", (uintptr_t)&sleep },
        { "srand48", (uintptr_t)&srand48 },
        { "strtod", (uintptr_t)&strtod },
        { "strtof", (uintptr_t)&strtof },
        { "strtoimax", (uintptr_t)&strtoimax },
        { "strtol", (uintptr_t)&strtol },
        { "strtold", (uintptr_t)&strtold },
        { "strtoll", (uintptr_t)&strtoll },
        { "strtoul", (uintptr_t)&strtoul },
        { "strtoull", (uintptr_t)&strtoull },
        { "strtoumax", (uintptr_t)&strtoumax },
        { "usleep", (uintptr_t)&usleep },

        #ifdef USE_SCELIBC_IO
            { "qsort", (uintptr_t)&sceLibcBridge_qsort },
            { "rand", (uintptr_t)&sceLibcBridge_rand },
            { "srand", (uintptr_t)&sceLibcBridge_srand },
        #else
            { "qsort", (uintptr_t)&qsort },
            { "rand", (uintptr_t)&rand },
            { "srand", (uintptr_t)&srand },
        #endif


        // Env
        { "getenv", (uintptr_t)&getenv_soloader },
        { "setenv", (uintptr_t)&setenv_soloader },


        // Jmp
        { "setjmp", (uintptr_t)&setjmp }, // TODO: May have different struct size?
        { "longjmp", (uintptr_t)&longjmp }, // TODO: May have different struct size?


        // Signals
        { "bsd_signal", (uintptr_t)&signal },
        { "raise", (uintptr_t)&raise },
        { "sigaction", (uintptr_t)&sigaction },


        // Locale
        { "freelocale", (uintptr_t)&freelocale },
        { "localeconv", (uintptr_t)&localeconv },
        { "newlocale", (uintptr_t)&newlocale },
        { "setlocale", (uintptr_t)&setlocale },
        { "uselocale", (uintptr_t)&uselocale },


        // zlib
        { "adler32", (uintptr_t)&adler32 },
        { "compress", (uintptr_t)&compress },
        { "compressBound", (uintptr_t)&compressBound },
        { "crc32", (uintptr_t)&crc32 },
        { "deflate", (uintptr_t)&deflate },
        { "deflateEnd", (uintptr_t)&deflateEnd },
        { "deflateInit2_", (uintptr_t)&deflateInit2_ },
        { "deflateInit_", (uintptr_t)&deflateInit_ },
        { "deflateReset", (uintptr_t)&deflateReset },
        { "gzclose", (uintptr_t)&gzclose },
        { "gzgets", (uintptr_t)&gzgets },
        { "gzopen", (uintptr_t)&gzopen },
        { "inflate", (uintptr_t)&inflate },
        { "inflateEnd", (uintptr_t)&inflateEnd },
        { "inflateInit2_", (uintptr_t)&inflateInit2_ },
        { "inflateInit_", (uintptr_t)&inflateInit_ },
        { "inflateReset", (uintptr_t)&inflateReset },
        { "inflateReset2", (uintptr_t)&inflateReset2 },
        { "uncompress", (uintptr_t)&uncompress },
};

// Unity-specific entries (na_dynlib.c) take precedence over this table.
extern so_default_dynlib na_dynlib[];
extern const int na_dynlib_count;

static so_default_dynlib *merged = NULL;
static int merged_count = 0;

static void build_merged(void) {
    if (merged) return;
    int n_default = sizeof(default_dynlib) / sizeof(default_dynlib[0]);
    merged = malloc(sizeof(so_default_dynlib) * (na_dynlib_count + n_default));
    for (int i = 0; i < na_dynlib_count; i++)
        merged[merged_count++] = na_dynlib[i];
    for (int i = 0; i < n_default; i++)
        merged[merged_count++] = default_dynlib[i];
}

void *na_dynlib_lookup(const char *symbol) {
    build_merged();
    for (int i = 0; i < merged_count; i++)
        if (strcmp(symbol, merged[i].symbol) == 0)
            return (void *)merged[i].func;
    return NULL;
}

void resolve_imports(so_module* mod) {
    __sF_fake[0] = *stdin;
    __sF_fake[1] = *stdout;
    __sF_fake[2] = *stderr;
    build_merged();
    // 0: also link against the other game libraries (DT_NEEDED)
    so_resolve(mod, merged, merged_count * sizeof(so_default_dynlib), 0);
}
