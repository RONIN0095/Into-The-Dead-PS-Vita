/*
 * Copyright (C) 2026 Vita port contributors
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

/**
 * @file  na_dynlib.c
 * @brief Imports of libunity.so/libil2cpp.so that the generic table in
 *        dynlib.c does not cover (or covers wrongly for Unity).
 *
 * Entries here take precedence over dynlib.c. docs/imports/ lists every
 * import of both libraries; tools/check_imports.py checks that each one is
 * in one of the two tables.
 */

#include <ctype.h>
#include <errno.h>
#include <math.h>
#include <setjmp.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <wchar.h>
#include <wctype.h>
#include <malloc.h>
#include <pthread.h>
#include <sys/time.h>

#include <psp2/kernel/clib.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/rtc.h>

#include <so_util/so_util.h>

#include <SLES/OpenSLES.h>
#include <SLES/OpenSLES_Android.h>

#include "na_modules.h"
#include "na_threads.h"
#include "na_stats.h"
#include "na_filecache.h"
#include "na_apk.h"
#include "na_android.h"
#include "na_egl.h"
#include "utils/logger.h"
#include "utils/utils.h"
#include "reimpl/errno.h"
#include "reimpl/pthr.h"
#include "reimpl/io.h"
#include "reimpl/sys.h"

/*
 * stdio: bionic's __sF[3] and the stdout/stderr variables (API 23+).
 * The libraries pass these FILE pointers to fprintf/fwrite/...; the wrappers
 * below turn them into newlib's streams.
 */
#define BIONIC_FILE_SIZE 84
static uint8_t sF_bionic[3][BIONIC_FILE_SIZE];
static FILE *stdin_bionic = (FILE *)sF_bionic[0];
static FILE *stdout_bionic = (FILE *)sF_bionic[1];
static FILE *stderr_bionic = (FILE *)sF_bionic[2];

static FILE *fix_file(FILE *f) {
    uintptr_t p = (uintptr_t)f, b = (uintptr_t)sF_bionic;
    if (p >= b && p < b + sizeof(sF_bionic)) {
        switch ((p - b) / BIONIC_FILE_SIZE) {
            case 0: return stdin;
            case 1: return stdout;
            default: return stderr;
        }
    }
    return f;
}

static int is_std_stream(FILE *f) {
    f = fix_file(f);
    return f == stdout || f == stderr;
}

// Output to stdout/stderr goes to the log
static int vfprintf_na(FILE *f, const char *fmt, va_list ap) {
    if (is_std_stream(f)) {
        char buf[1024];
        int r = vsnprintf(buf, sizeof(buf), fmt, ap);
        l_info("[stdio] %s", buf);
        return r;
    }
    return vfprintf(fix_file(f), fmt, ap);
}

static int fprintf_na(FILE *f, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int r = vfprintf_na(f, fmt, ap);
    va_end(ap);
    return r;
}

static size_t fwrite_na(const void *p, size_t sz, size_t n, FILE *f) {
    if (is_std_stream(f)) {
        size_t len = sz * n;
        char buf[1024];
        if (len >= sizeof(buf)) len = sizeof(buf) - 1;
        memcpy(buf, p, len);
        buf[len] = 0;
        l_info("[stdio] %s", buf);
        return n;
    }
    return fwrite(p, sz, n, fix_file(f));
}

static int fputs_na(const char *s, FILE *f) {
    if (is_std_stream(f)) {
        l_info("[stdio] %s", s);
        return 1;
    }
    return fputs(s, fix_file(f));
}

static int fputc_na(int c, FILE *f) {
    if (is_std_stream(f)) return c;
    return fputc(c, fix_file(f));
}

static int fflush_na(FILE *f) { return f ? fflush(fix_file(f)) : 0; }
static int fileno_na(FILE *f) { return fileno(fix_file(f)); }
static int ferror_na(FILE *f) { return ferror(fix_file(f)); }
static int feof_na(FILE *f) { return feof(fix_file(f)); }
static void clearerr_na(FILE *f) { clearerr(fix_file(f)); }
static void setbuf_na(FILE *f, char *buf) { setbuf(fix_file(f), buf); }

static int puts_na(const char *s) {
    l_info("[stdio] %s", s);
    return 1;
}

static int printf_na(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int r = vfprintf_na(stdout, fmt, ap);
    va_end(ap);
    return r;
}

static int vprintf_na(const char *fmt, va_list ap) {
    return vfprintf_na(stdout, fmt, ap);
}

/*
 * System properties
 */
typedef struct {
    const char *name;
    const char *value;
} prop_entry;

static const prop_entry props[] = {
    { "ro.build.version.sdk", "28" },
    { "ro.build.version.release", "9" },
    { "ro.product.model", "PS Vita" },
    { "ro.product.manufacturer", "Sony" },
    { "ro.product.brand", "Sony" },
    { "ro.product.device", "vita" },
    { "ro.product.name", "vita" },
    { "ro.product.board", "vita" },
    { "ro.hardware", "vita" },
    { "ro.board.platform", "vita" },
    { "ro.product.cpu.abi", "armeabi-v7a" },
    { "ro.product.cpu.abilist", "armeabi-v7a,armeabi" },
    { "ro.product.cpu.abilist32", "armeabi-v7a,armeabi" },
    { "ro.product.cpu.abilist64", "" },
    { "ro.build.fingerprint", "Sony/vita/vita:9/PSVITA/1:user/release-keys" },
    { "ro.build.tags", "release-keys" },
    { "ro.build.type", "user" },
    { "ro.debuggable", "0" },
    { "ro.kernel.qemu", "0" },
    { "ro.opengles.version", "196608" }, // 3.0
    { "ro.sf.lcd_density", "220" },
    { "ro.config.low_ram", "true" },
    { "persist.sys.locale", "en-US" },
    { "ro.product.locale", "en-US" },
    { "debug.unity.no-strict-mode", "1" },
};

static const prop_entry *find_prop(const char *name) {
    for (unsigned i = 0; i < sizeof(props) / sizeof(props[0]); i++)
        if (strcmp(props[i].name, name) == 0) return &props[i];
    return NULL;
}

static int system_property_get(const char *name, char *value) {
    const prop_entry *p = find_prop(name);
    if (!p) {
        value[0] = 0;
        l_debug("__system_property_get(%s): (unset)", name);
        return 0;
    }
    strcpy(value, p->value);
    l_debug("__system_property_get(%s): %s", name, value);
    return strlen(value);
}

static const void *system_property_find(const char *name) {
    return find_prop(name);
}

static int system_property_read(const void *pi, char *name, char *value) {
    const prop_entry *p = pi;
    if (!p) return 0;
    if (name) strcpy(name, p->name);
    strcpy(value, p->value);
    return strlen(p->value);
}

/*
 * sysconf with bionic's constants
 */
#define BIONIC_SC_CLK_TCK 0x0006
#define BIONIC_SC_PAGESIZE 0x0027
#define BIONIC_SC_PAGE_SIZE 0x0028
#define BIONIC_SC_NPROCESSORS_CONF 0x0060
#define BIONIC_SC_NPROCESSORS_ONLN 0x0061
#define BIONIC_SC_PHYS_PAGES 0x0062
#define BIONIC_SC_AVPHYS_PAGES 0x0063

static long sysconf_na(int name) {
    switch (name) {
        case BIONIC_SC_CLK_TCK: return 100;
        case BIONIC_SC_PAGESIZE:
        case BIONIC_SC_PAGE_SIZE: return 4096;
        // Three cores are usable by applications
        case BIONIC_SC_NPROCESSORS_CONF:
        case BIONIC_SC_NPROCESSORS_ONLN: return 3;
        case BIONIC_SC_PHYS_PAGES: return (512 * 1024 * 1024) / 4096;
        case BIONIC_SC_AVPHYS_PAGES: return (256 * 1024 * 1024) / 4096;
        default:
            l_warn("sysconf(0x%x): unknown, returning -1", name);
            errno = EINVAL;
            return -1;
    }
}

/*
 * Files
 */
const char *na_io_fd_name(int fd);
extern volatile int na_io_trace_left;

static long long lseek64_na(int fd, long long off, int whence) {
    long long r = lseek(fd, (off_t)off, whence);
    if (na_io_fd_name(fd) && na_io_trace_left > 0) {
        na_io_trace_left--;
        l_info("[IO] t%x lseek64(%d %s, %lld, %d) = %lld", sceKernelGetThreadId(), fd, na_io_fd_name(fd), off, whence, r);
    }
    if (r < 0) l_warn("lseek64(%d, %lld, %d): %lld", fd, off, whence, r);
    return r;
}

static long lseek_na(int fd, long off, int whence) {
    long r = lseek(fd, off, whence);
    if (na_io_fd_name(fd) && na_io_trace_left > 0) {
        na_io_trace_left--;
        l_info("[IO] t%x lseek(%d %s, %ld, %d) = %ld", sceKernelGetThreadId(), fd, na_io_fd_name(fd), off, whence, r);
    }
    if (r < 0) l_warn("lseek(%d, %ld, %d): %ld", fd, off, whence, r);
    return r;
}

typedef struct {
    uint32_t f_type;
    uint32_t f_bsize;
    uint64_t f_blocks;
    uint64_t f_bfree;
    uint64_t f_bavail;
    uint64_t f_files;
    uint64_t f_ffree;
    int32_t f_fsid[2];
    uint32_t f_namelen;
    uint32_t f_frsize;
    uint32_t f_flags;
    uint32_t f_spare[4];
} statfs_bionic;

static int statfs_na(const char *path, statfs_bionic *st) {
    path = na_fix_path(path);
    l_info("statfs(%s)", path ? path : "(null)");
    memset(st, 0, sizeof(*st));
    st->f_type = 0xEF53;
    st->f_bsize = st->f_frsize = 4096;
    st->f_blocks = (4ULL << 30) / 4096;
    st->f_bfree = st->f_bavail = (2ULL << 30) / 4096;
    st->f_files = st->f_ffree = 65536;
    st->f_namelen = 255;
    return 0;
}

/*
 * File copying: IL2CPP copies its metadata with sendfile() and may size the
 * target with ftruncate() first; neither exists in newlib on the Vita.
 */
static long sendfile_na(int out_fd, int in_fd, long *offset, size_t count) {
    static char buf[64 * 1024];
    off_t saved = 0;
    if (offset) {
        saved = lseek(in_fd, 0, SEEK_CUR);
        lseek(in_fd, *offset, SEEK_SET);
    }
    size_t done = 0;
    while (done < count) {
        size_t n = count - done < sizeof(buf) ? count - done : sizeof(buf);
        int r = read(in_fd, buf, n);
        if (r <= 0) break;
        int w = write(out_fd, buf, r);
        if (w != r) {
            if (done == 0) {
                errno = EIO;
                return -1;
            }
            break;
        }
        done += r;
    }
    if (offset) {
        *offset += done;
        lseek(in_fd, saved, SEEK_SET);
    }
    static int logged = 0;
    if (logged++ < 16) l_info("sendfile(%d -> %d, %u bytes): %u", in_fd, out_fd, (unsigned)count, (unsigned)done);
    return (long)done;
}

static int ftruncate_na(int fd, long len) {
    struct stat st;
    if (fstat(fd, &st) != 0) return -1;
    if (len == st.st_size) return 0;
    if (len > st.st_size) {
        // grow: append zeros
        static char zeros[16 * 1024];
        off_t pos = lseek(fd, 0, SEEK_CUR);
        lseek(fd, 0, SEEK_END);
        long left = len - st.st_size;
        while (left > 0) {
            int n = left < (long)sizeof(zeros) ? (int)left : (int)sizeof(zeros);
            if (write(fd, zeros, n) != n) {
                lseek(fd, pos, SEEK_SET);
                errno = ENOSPC;
                return -1;
            }
            left -= n;
        }
        lseek(fd, pos, SEEK_SET);
        return 0;
    }
    l_warn("ftruncate(%d, %ld): shrinking from %ld is not supported, ignored", fd, len, (long)st.st_size);
    return 0;
}

static int truncate_na(const char *path, long len) {
    path = na_fix_path(path);
    int fd = open(path, O_WRONLY);
    if (fd < 0) return -1;
    int r = ftruncate_na(fd, len);
    close(fd);
    return r;
}

/*
 * Tracing of the first file reads/writes (to see how Unity copies files)
 */
static int io_trace_left = 300;

static long read_na(int fd, void *buf, size_t n) {
    long cr;
    if (na_io_trace_left <= 0 && na_filecache_read(fd, buf, n, &cr)) return cr;
    long pos = na_io_fd_name(fd) ? (long)lseek(fd, 0, SEEK_CUR) : -1;
    uint64_t t0 = sceKernelGetProcessTimeWide();
    long r = read(fd, buf, n);
    na_stats.read_us += (uint32_t)(sceKernelGetProcessTimeWide() - t0);
    if (pos >= 0) na_filecache_game_read(); // an asset file or the APK: reading ahead waits
    if (r > 0) na_stats.read_kb += (uint32_t)(r / 1024);
    /*
     * A read that stops at the end of a file leaves the Vita's file position
     * past the end (by what was asked minus 256 KB: reading 3 MB from a 1 MB
     * split left it at 2.75 MB). Unity reads its split assets that way and
     * then asks for the position, so it skipped the next split and big
     * textures lost half their data (black title, black cutscene pictures,
     * flat-coloured scenery). A short read on a file means its end: put the
     * position there, as Linux does. (On pipes and sockets lseek just fails.)
     */
    if (r >= 0 && (size_t)r < n) lseek(fd, 0, SEEK_END);
    if (na_io_fd_name(fd) && na_io_trace_left > 0) {
        na_io_trace_left--;
        unsigned zero = 0, words = 0;
        for (long i = 0; i + 4 <= r; i += 64, words++)
            if (*(const uint32_t *)((const uint8_t *)buf + i) == 0) zero++;
        l_info("[IO] t%x read(%d %s @%ld, %u) = %ld, %u%% zero", sceKernelGetThreadId(), fd, na_io_fd_name(fd), pos,
               (unsigned)n, r, words ? zero * 100 / words : 0);
    }
    static int short_logged = 0;
    if (r >= 0 && (size_t)r < n && n >= 65536 && short_logged < 40) {
        short_logged++;
        l_info("read(%d, %u): short read %ld", fd, (unsigned)n, r);
    }
    if (r < 0 && io_trace_left > 0) { io_trace_left--; l_warn("read(%d, %u): %ld", fd, (unsigned)n, r); }
    return r;
}

static long write_na(int fd, const void *buf, size_t n) {
    long r = write(fd, buf, n);
    if (fd >= 3 && r != (long)n && io_trace_left > 0) { io_trace_left--; l_warn("write(%d, %u): %ld", fd, (unsigned)n, r); }
    return r;
}

#define FIX(p) char fixbuf_##p[512]; p = na_apk_map_path(p, fixbuf_##p, sizeof(fixbuf_##p))

static int rename_na(const char *a, const char *b) {
    FIX(a);
    FIX(b);
    int r = rename(a, b);
    l_info("rename(%s, %s): %d", a, b, r);
    return r;
}

static int mkdir_na(const char *p, int mode) {
    FIX(p);
    int r = mkdir(p, mode);
    if (io_trace_left > 0) { io_trace_left--; l_info("mkdir(%s): %d", p, r); }
    return r;
}

static int unlink_na(const char *p) {
    FIX(p);
    int r = unlink(p);
    if (io_trace_left > 0) { io_trace_left--; l_info("unlink(%s): %d", p, r); }
    return r;
}

static int rmdir_na(const char *p) {
    FIX(p);
    int r = rmdir(p);
    if (io_trace_left > 0) { io_trace_left--; l_info("rmdir(%s): %d", p, r); }
    return r;
}

// realpath: the (Android) path stays as given, if it exists after mapping
static char *realpath_na(const char *path, char *resolved) {
    char mb[512];
    struct stat st;
    const char *m = na_apk_map_path(path, mb, sizeof(mb));
    if (!path || stat(m, &st) != 0) {
        errno = ENOENT;
        return NULL;
    }
    if (!resolved) resolved = malloc(4096);
    snprintf(resolved, 4096, "%s", path);
    return resolved;
}

static int readlink_na(const char *path, char *buf, size_t sz) {
    l_debug("readlink(%s): not supported", path);
    errno = EINVAL;
    return -1;
}

static int fail_enosys(void) {
    errno = ENOSYS;
    return -1;
}

/*
 * Users
 */
typedef struct {
    char *pw_name;
    char *pw_passwd;
    uint32_t pw_uid;
    uint32_t pw_gid;
    char *pw_dir;
    char *pw_shell;
} passwd_bionic;

#define APP_UID 10123

static passwd_bionic the_user = {
    "u0_a123", "", APP_UID, APP_UID, DATA_PATH, "/system/bin/sh"
};

static passwd_bionic *getpwuid_na(uint32_t uid) { return &the_user; }

static int getpwuid_r_na(uint32_t uid, passwd_bionic *pw, char *buf, size_t len, passwd_bionic **res) {
    *pw = the_user;
    *res = pw;
    return 0;
}

static int get_uid(void) { return APP_UID; }
static int get_pid(void) { return 4242; }
static int get_tid(void) { return sceKernelGetThreadId(); }

/*
 * Threads
 */
// Stack bounds of the calling thread (the Boehm GC scans them)
static int pthread_getattr_np_na(pthread_t thread, pthread_attr_t_bionic *attr) {
    SceKernelThreadInfo info;
    memset(&info, 0, sizeof(info));
    info.size = sizeof(info);
    if (sceKernelGetThreadInfo(sceKernelGetThreadId(), &info) < 0)
        return ESRCH;
    if (thread != pthread_self())
        l_warn("pthread_getattr_np: other thread, returning the caller's stack");
    memset(attr, 0, sizeof(*attr));
    // magic/real_ptr are not used by the functions below
    attr->real_ptr = NULL;
    attr->magic = (int32_t)(uintptr_t)info.stack; // stack_base (bionic layout)
    attr->stack_size = info.stackSize;
    attr->guard_size = 4096;
    return 0;
}

static int pthread_attr_getstack_na(const pthread_attr_t_bionic *attr, void **addr, size_t *size) {
    *addr = (void *)(uintptr_t)attr->magic;
    *size = attr->stack_size;
    return 0;
}

static int pthread_condattr_setclock_na(pthread_condattr_t *attr, int clock) {
    return 0;
}

// Condition variables created with CLOCK_MONOTONIC get monotonic deadlines;
// newlib's pthread waits on realtime. Detect and convert.
static int pthread_cond_timedwait_na(pthread_cond_t_bionic *cond, pthread_mutex_t_bionic *mutex,
                                     const struct timespec *abstime) {
    struct timespec rt;
    if (abstime && abstime->tv_sec < 946684800) { // before 2000: a monotonic deadline
        struct timespec now_mono, now_rt;
        clock_gettime_soloader(1 /* CLOCK_MONOTONIC */, &now_mono);
        clock_gettime_soloader(0 /* CLOCK_REALTIME */, &now_rt);
        int64_t delta = (int64_t)(abstime->tv_sec - now_mono.tv_sec) * 1000000000LL +
                        (abstime->tv_nsec - now_mono.tv_nsec);
        if (delta < 0) delta = 0;
        int64_t t = (int64_t)now_rt.tv_sec * 1000000000LL + now_rt.tv_nsec + delta;
        rt.tv_sec = t / 1000000000LL;
        rt.tv_nsec = t % 1000000000LL;
        abstime = &rt;
    }
    return pthread_cond_timedwait_soloader(cond, mutex, (struct timespec *)abstime);
}

// bionic pthread_rwlock_t is 40 bytes; the first word points to ours.
static SceKernelLwMutexWork rwlock_init_lock;
static int rwlock_init_lock_ready = 0;

static pthread_rwlock_t *rwlock_get(void *lock) {
    pthread_rwlock_t **slot = (pthread_rwlock_t **)lock;
    if (*slot) return *slot;
    sceKernelLockLwMutex(&rwlock_init_lock, 1, NULL);
    if (!*slot) {
        pthread_rwlock_t *l = malloc(sizeof(pthread_rwlock_t));
        pthread_rwlock_init(l, NULL);
        *slot = l;
    }
    sceKernelUnlockLwMutex(&rwlock_init_lock, 1);
    return *slot;
}

static int rwlock_init_na(void *lock, const void *attr) {
    memset(lock, 0, 40);
    rwlock_get(lock);
    return 0;
}
static int rwlock_rdlock_na(void *lock) { return pthread_rwlock_rdlock(rwlock_get(lock)); }
static int rwlock_wrlock_na(void *lock) { return pthread_rwlock_wrlock(rwlock_get(lock)); }
static int rwlock_unlock_na(void *lock) { return pthread_rwlock_unlock(rwlock_get(lock)); }

static int pthread_kill_na(pthread_t t, int sig) {
    // The Boehm GC suspends threads with signals (SIGPWR/SIGXCPU) and then
    // waits for each one to acknowledge from its signal handler. There are no
    // signals on the Vita, so "sent" signals hung that wait forever (Unity
    // stops the world for UnloadUnusedAssets while loading a scene). ESRCH
    // ("no such thread") is a case the GC handles: it skips the thread.
    // Collection is off (GC_DONT_GC), so only Unity's liveness scan sees less.
    if (sig == 0) return 0;
    static int warned = 0;
    if (warned++ < 4) l_warn("pthread_kill(%p, %d): no signals here, reporting ESRCH", (void *)t, sig);
    return 3; // ESRCH (bionic)
}

/*
 * Signals (accepted and ignored)
 */
typedef unsigned long sigset_bionic;

static int sigemptyset_na(sigset_bionic *s) { *s = 0; return 0; }
static int sigfillset_na(sigset_bionic *s) { *s = ~0UL; return 0; }
static int sigaddset_na(sigset_bionic *s, int n) { if (n > 0 && n <= 32) *s |= 1UL << (n - 1); return 0; }
static int sigdelset_na(sigset_bionic *s, int n) { if (n > 0 && n <= 32) *s &= ~(1UL << (n - 1)); return 0; }

/*
 * Fault signals. Handlers are recorded; SIGSEGV/SIGBUS handlers really get
 * called: kubridge passes data/prefetch aborts to kuKernelRegisterAbortHandler,
 * and the faulting thread is sent into the handler, like on Linux. IL2CPP's
 * Boehm GC needs this: GC_find_limit reads memory until it faults and
 * longjmps out of its SIGSEGV handler. Other signals are only recorded.
 */
#include <kubridge.h>

#define BIONIC_SIGBUS 7
#define BIONIC_SIGSEGV 11
#define BIONIC_SA_SIGINFO 4
#define NSIG_BIONIC 65

typedef struct {
    void *handler;
    uint32_t mask;
    int flags;
    void *restorer;
} sigaction_bionic;

static sigaction_bionic sig_actions[NSIG_BIONIC];
static volatile int abort_handler_on = 0;
static KuKernelAbortHandler old_abort_handler = NULL;



/*
 * Signal frames. The handler gets (sig, siginfo, ucontext) like on Linux;
 * when it returns, na_sigreturn restores the registers from the ucontext,
 * which the handler may have changed (IL2CPP turns faults into managed
 * NullReferenceExceptions that way). If it changed nothing, the fault is
 * not handled: delivery is switched off and the faulting instruction runs
 * again, so the crash happens for real and the core dump shows its place.
 *
 * bionic ARM ucontext: uc_flags, uc_link, uc_stack (12 bytes), then
 * uc_mcontext = sigcontext { trap_no, error_code, oldmask, r0..r10, fp, ip,
 * sp, lr, pc, cpsr, fault_address } at offset 20.
 */
#define UC_R0 8      // word index of arm_r0 in the ucontext
#define UC_SP (UC_R0 + 13)
#define UC_LR (UC_R0 + 14)
#define UC_PC (UC_R0 + 15)
#define UC_CPSR (UC_R0 + 16)
#define UC_FAULT (UC_R0 + 17)
#define UC_WORDS 192 // 768 bytes: room for uc_sigmask/uc_regspace
#define UC_ORIG_PC (UC_WORDS - 1)
#define N_FRAMES 8

typedef struct {
    uint32_t uc[UC_WORDS];
    uint32_t info[32];
} sig_frame;

static sig_frame frames[N_FRAMES];
static volatile int next_frame = 0;
static volatile uint32_t last_fault_pc, last_fault_addr;

// Called by na_sigreturn with the frame (r4 still holds it after the handler)
uint32_t *na_sig_after(sig_frame *f) {
    uint32_t *uc = f->uc;
    if (uc[UC_PC] == uc[UC_ORIG_PC]) {
        // not handled: crash for real at the original instruction
        kuKernelReleaseAbortHandler();
        abort_handler_on = 0;
        memset(sig_actions, 0, sizeof(sig_actions));
        l_fatal("Unhandled fault at pc 0x%08x (address 0x%08x); core dump follows.", uc[UC_PC], uc[UC_FAULT]);
    }
    return uc;
}

// r0 = ucontext: restore r0-r12, sp, lr, pc, flags and continue there
__attribute__((naked, target("arm"))) void na_sig_restore(uint32_t *uc) {
    __asm__ volatile (
        "ldr lr, [r0, #88]\n"     // lr  (word 22)
        "ldr r1, [r0, #84]\n"     // sp  (word 21)
        "ldr r2, [r0, #92]\n"     // pc  (word 23)
        "ldr r3, [r0, #96]\n"     // cpsr (word 24)
        "tst r3, #0x20\n"
        "orrne r2, r2, #1\n"      // Thumb target
        "sub r1, r1, #8\n"
        "str r2, [r1, #4]\n"
        "ldr r2, [r0, #32]\n"     // r0 (word 8)
        "str r2, [r1]\n"
        "msr APSR_nzcvq, r3\n"
        "mov sp, r1\n"
        "add r0, r0, #36\n"       // r1.. (word 9)
        "ldm r0, {r1-r12}\n"
        "pop {r0, pc}\n"
    );
}

// The handler returns here; r4 = frame (callee-saved, set when redirecting)
__attribute__((naked, target("arm"))) void na_sigreturn(void) {
    __asm__ volatile (
        "mov r0, r4\n"
        "bl na_sig_after\n"
        "b na_sig_restore\n"
    );
}

int na_mmap_fault(uint32_t addr); // reimpl/mem.c
static volatile int faults_wanted = 0;

static void abort_handler(KuKernelAbortContext *ctx) {
    // a reserved page touched for the first time: it gets memory, and the
    // instruction runs again (reimpl/mem.c)
    if (faults_wanted && na_mmap_fault(ctx->FAR)) return;
    int sig = BIONIC_SIGSEGV;
    void *h = sig_actions[sig].handler;
    if ((uintptr_t)h <= 1) {
        sig = BIONIC_SIGBUS;
        h = sig_actions[sig].handler;
    }
    // Only handlers inside libil2cpp get faults (Boehm's GC_find_limit,
    // IL2CPP's NullReferenceException). Unity's crash reporter would only
    // hide where the crash happened.
    NaLib *owner = na_module_for_addr((uintptr_t)h & ~1u);
    if ((uintptr_t)h <= 1 || !owner || owner != &na_libs[NA_LIB_IL2CPP]) {
        last_fault_pc = ctx->pc;
        last_fault_addr = ctx->FAR;
        kuKernelReleaseAbortHandler();
        abort_handler_on = 0;
        return;
    }

    sig_frame *f = &frames[__sync_fetch_and_add(&next_frame, 1) % N_FRAMES];
    memset(f, 0, sizeof(*f));
    uint32_t *uc = f->uc;
    const uint32_t *regs = &ctx->r0;
    for (int i = 0; i < 16; i++) uc[UC_R0 + i] = regs[i]; // r0..r12, sp, lr, pc
    uc[UC_CPSR] = ctx->SPSR;
    uc[UC_FAULT] = ctx->FAR;
    uc[UC_ORIG_PC] = ctx->pc;
    uc[UC_R0 - 3] = 14; // trap_no: data abort
    f->info[0] = sig;
    f->info[2] = 1; // SEGV_MAPERR
    f->info[3] = ctx->FAR;

    ctx->r0 = sig;
    ctx->r1 = (uint32_t)(uintptr_t)f->info;
    ctx->r2 = (uint32_t)(uintptr_t)uc;
    ctx->r4 = (uint32_t)(uintptr_t)f;
    ctx->lr = (uint32_t)(uintptr_t)&na_sigreturn;
    ctx->sp = (ctx->sp - 128) & ~7u;
    uint32_t pc = (uint32_t)(uintptr_t)h;
    if (pc & 1) {
        ctx->SPSR |= 0x20; // Thumb
        pc &= ~1u;
    } else {
        ctx->SPSR &= ~0x20u;
    }
    ctx->SPSR &= ~0x0600FC00u; // clear IT state bits
    ctx->pc = pc;
}

static void update_abort_handler(void) {
    int want = faults_wanted || (uintptr_t)sig_actions[BIONIC_SIGSEGV].handler > 1 ||
               (uintptr_t)sig_actions[BIONIC_SIGBUS].handler > 1;
    if (want && !abort_handler_on) {
        KuKernelAbortHandlerOpt opt;
        memset(&opt, 0, sizeof(opt));
        opt.size = sizeof(opt);
        int r = kuKernelRegisterAbortHandler(abort_handler, &old_abort_handler, &opt);
        if (r < 0) {
            static int warned = 0;
            if (!warned++) l_error("kuKernelRegisterAbortHandler: 0x%x (kubridge too old? Need v0.3+)", r);
            return;
        }
        abort_handler_on = 1;
    } else if (!want && abort_handler_on) {
        kuKernelReleaseAbortHandler();
        abort_handler_on = 0;
    }
}

// reimpl/mem.c, at the first reservation: faults are needed from now on
void na_faults_enable(void) {
    if (faults_wanted) return;
    faults_wanted = 1;
    update_abort_handler();
}

static int sigaction_na(int sig, const sigaction_bionic *act, sigaction_bionic *old) {
    if (sig <= 0 || sig >= NSIG_BIONIC) {
        errno = EINVAL;
        return -1;
    }
    if (old) *old = sig_actions[sig];
    if (act) {
        sig_actions[sig] = *act;
        l_debug("sigaction(%d, handler %p, flags 0x%x)", sig, act->handler, act->flags);
        if (sig == BIONIC_SIGSEGV || sig == BIONIC_SIGBUS) update_abort_handler();
    }
    return 0;
}

static void *signal_na(int sig, void *handler) {
    if (sig <= 0 || sig >= NSIG_BIONIC) return (void *)-1;
    void *old = sig_actions[sig].handler;
    sig_actions[sig].handler = handler;
    sig_actions[sig].flags = 0;
    if (sig == BIONIC_SIGSEGV || sig == BIONIC_SIGBUS) update_abort_handler();
    return old;
}

static int sigsuspend_na(const sigset_bionic *mask) {
    l_warn("sigsuspend: no signals, returning EINTR");
    errno = EINTR;
    return -1;
}

static int sigprocmask_na(int how, const sigset_bionic *set, sigset_bionic *old) {
    if (old) *old = 0;
    return 0;
}

/*
 * Misc
 */
static int posix_memalign_na(void **out, size_t align, size_t size) {
    if (align < sizeof(void *)) align = sizeof(void *);
    void *p = memalign(align, size);
    if (!p) return ENOMEM;
    *out = p;
    return 0;
}

static int fd_isset_chk(int fd, const void *set, size_t sz) {
    return (((const uint32_t *)set)[fd / 32] >> (fd % 32)) & 1;
}

static void fd_set_chk(int fd, void *set, size_t sz) {
    ((uint32_t *)set)[fd / 32] |= 1u << (fd % 32);
}

static size_t ctype_get_mb_cur_max(void) { return 1; }

static char *gnu_strerror_r(int err, char *buf, size_t len) {
    strncpy(buf, strerror(err), len);
    if (len) buf[len - 1] = 0;
    return buf;
}

static void android_set_abort_message_na(const char *msg) {
    l_fatal("Abort message: %s", msg ? msg : "(null)");
}

static void syslog_na(int prio, const char *fmt, ...) {
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    l_info("[syslog] %s", buf);
}

static int isatty_na(int fd) { return 0; }
static unsigned if_nametoindex_na(const char *n) { return 0; }
static uint32_t inet_addr_na(const char *s) { return 0xffffffff; }

typedef struct { int quot; int rem; } div_bionic;
static div_bionic div_na(int a, int b) {
    div_bionic r = { a / b, a % b };
    return r;
}

// Locale variants: the libraries only ever pass the C/UTF-8 locale
static int iswalpha_l_na(wint_t c, void *l) { return iswalpha(c); }
static int iswblank_l_na(wint_t c, void *l) { return iswblank(c); }
static int iswcntrl_l_na(wint_t c, void *l) { return iswcntrl(c); }
static int iswdigit_l_na(wint_t c, void *l) { return iswdigit(c); }
static int iswlower_l_na(wint_t c, void *l) { return iswlower(c); }
static int iswprint_l_na(wint_t c, void *l) { return iswprint(c); }
static int iswpunct_l_na(wint_t c, void *l) { return iswpunct(c); }
static int iswspace_l_na(wint_t c, void *l) { return iswspace(c); }
static int iswupper_l_na(wint_t c, void *l) { return iswupper(c); }
static int iswxdigit_l_na(wint_t c, void *l) { return iswxdigit(c); }
static wint_t towlower_l_na(wint_t c, void *l) { return towlower(c); }
static wint_t towupper_l_na(wint_t c, void *l) { return towupper(c); }
static int strcoll_l_na(const char *a, const char *b, void *l) { return strcmp(a, b); }
static int wcscoll_l_na(const wchar_t *a, const wchar_t *b, void *l) { return wcscmp(a, b); }
static size_t strxfrm_l_na(char *d, const char *s, size_t n, void *l) { return strxfrm(d, s, n); }
static size_t wcsxfrm_l_na(wchar_t *d, const wchar_t *s, size_t n, void *l) { return wcsxfrm(d, s, n); }
static size_t strftime_l_na(char *s, size_t n, const char *f, const struct tm *t, void *l) { return strftime(s, n, f, t); }
static double strtold_l_na(const char *s, char **e, void *l) { return strtod(s, e); }
static long long strtoll_l_na(const char *s, char **e, int b, void *l) { return strtoll(s, e, b); }
static unsigned long long strtoull_l_na(const char *s, char **e, int b, void *l) { return strtoull(s, e, b); }
static double wcstold_na(const wchar_t *s, wchar_t **e) { return wcstod(s, e); }

/*
 * Environment. GC_DONT_GC: IL2CPP's Boehm GC stops other threads with
 * signals for a collection; there are no signals on the Vita, so the first
 * collection would wait forever. The GC is switched off instead (see
 * docs/NOTES.md); na_modules.c also turns Unity's il2cpp_gc_* calls into
 * no-ops.
 */
#define MAX_ENV 32
static char *env_names[MAX_ENV];
static char *env_values[MAX_ENV];
static int n_env = 0;

static int env_find(const char *name) {
    for (int i = 0; i < n_env; i++)
        if (env_names[i] && strcmp(env_names[i], name) == 0) return i;
    return -1;
}

static int setenv_na(const char *name, const char *value, int overwrite) {
    if (!name || !value) return -1;
    int i = env_find(name);
    if (i >= 0) {
        if (!overwrite) return 0;
        free(env_values[i]);
        env_values[i] = strdup(value);
        return 0;
    }
    if (n_env >= MAX_ENV) return -1;
    env_names[n_env] = strdup(name);
    env_values[n_env] = strdup(value);
    n_env++;
    return 0;
}

static char *getenv_na(const char *name) {
    int i = name ? env_find(name) : -1;
    return i >= 0 ? env_values[i] : NULL;
}

static int unsetenv_na(const char *name) {
    int i = env_find(name);
    if (i >= 0) {
        free(env_names[i]);
        free(env_values[i]);
        env_names[i] = env_values[i] = NULL;
    }
    return 0;
}

/*
 * getauxval: CPU features (Cortex-A9: VFPv3 + NEON, no IDIV, no VFPv4)
 */
#define AT_HWCAP 16
#define AT_HWCAP2 26
#define AT_PAGESZ 6

static unsigned long getauxval_na(unsigned long type) {
    switch (type) {
        case AT_HWCAP:
            // HALF | THUMB | FAST_MULT | VFP | EDSP | NEON | VFPv3 | TLS
            return 0x2 | 0x4 | 0x10 | 0x40 | 0x80 | 0x1000 | 0x2000 | 0x8000;
        case AT_HWCAP2: return 0;
        case AT_PAGESZ: return 4096;
        default:
            errno = ENOENT;
            return 0;
    }
}

/*
 * Networking: the Vita's network stack is never started, and a DNS lookup on
 * it does not come back. curl (UnityWebRequest, used by ads and analytics when
 * a level starts) then waits for its resolver thread forever, and so does the
 * main thread waiting on the request. Report "no network" right away instead.
 */
#define EAI_NONAME_BIONIC 8

static int getaddrinfo_na(const char *node, const char *service, const void *hints, void **res) {
    static int logged = 0;
    if (logged++ < 16) l_info("getaddrinfo(%s, %s): no network", node ? node : "(null)", service ? service : "(null)");
    if (res) *res = NULL;
    return EAI_NONAME_BIONIC;
}

static void freeaddrinfo_na(void *ai) {}

static void *gethostbyname_na(const char *name) {
    static int logged = 0;
    if (logged++ < 16) l_info("gethostbyname(%s): no network", name ? name : "(null)");
    return NULL;
}

static int socket_na(int domain, int type, int protocol) {
    static int logged = 0;
    if (logged++ < 16) l_info("socket(%d, %d, %d): no network", domain, type, protocol);
    errno = ENETDOWN;
    return -1;
}

static int connect_na(int fd, const void *addr, int len) {
    errno = ENETUNREACH;
    return -1;
}

/*
 * syscall(): futex (Unity's baselib locks are built on it), gettid, CPU affinity
 */
#define NR_GETTID 224
#define NR_FUTEX 240
#define NR_SCHED_SETAFFINITY 241
#define NR_SCHED_GETAFFINITY 242

#define FUTEX_WAIT 0
#define FUTEX_WAKE 1
#define FUTEX_WAIT_BITSET 9
#define FUTEX_WAKE_BITSET 10
#define FUTEX_CLOCK_REALTIME 256

#define FUTEX_BUCKETS 64

static pthread_mutex_t futex_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t futex_cond[FUTEX_BUCKETS];
static int futex_ready = 0;

static pthread_cond_t *futex_bucket(volatile int *addr) {
    return &futex_cond[(((uintptr_t)addr) >> 2) % FUTEX_BUCKETS];
}

// Waiters of a bucket are all woken (futex allows spurious wakeups).
static long futex_na(volatile int *addr, int op, int val, const struct timespec *timeout) {
    int cmd = op & 0x7f;
    if (cmd == FUTEX_WAKE || cmd == FUTEX_WAKE_BITSET) {
        pthread_mutex_lock(&futex_lock);
        pthread_cond_broadcast(futex_bucket(addr));
        pthread_mutex_unlock(&futex_lock);
        return val;
    }
    if (cmd != FUTEX_WAIT && cmd != FUTEX_WAIT_BITSET) {
        l_warn("futex: op %d not supported", op);
        errno = ENOSYS;
        return -1;
    }

    // Deadline in process time (microseconds), 0 = none
    uint64_t start = sceKernelGetProcessTimeWide(), deadline = 0;
    if (timeout) {
        int64_t rel_us;
        if (cmd == FUTEX_WAIT) {
            rel_us = (int64_t)timeout->tv_sec * 1000000LL + timeout->tv_nsec / 1000;
        } else if (op & FUTEX_CLOCK_REALTIME) {
            struct timespec now;
            clock_gettime_soloader(0 /* CLOCK_REALTIME */, &now);
            rel_us = (int64_t)(timeout->tv_sec - now.tv_sec) * 1000000LL + (timeout->tv_nsec - now.tv_nsec) / 1000;
        } else {
            // absolute CLOCK_MONOTONIC, which is process time here
            rel_us = (int64_t)timeout->tv_sec * 1000000LL + timeout->tv_nsec / 1000 - (int64_t)start;
        }
        if (rel_us <= 0) {
            errno = ETIMEDOUT;
            return -1;
        }
        deadline = start + rel_us;
    }

    pthread_mutex_lock(&futex_lock);
    if (*addr != val) {
        pthread_mutex_unlock(&futex_lock);
        errno = EAGAIN;
        return -1;
    }
    // Wait in slices of at most 4 ms and look at the value in between, so a
    // lost wakeup costs 4 ms instead of a hang (returning early is allowed).
    // The slice's deadline uses newlib's CLOCK_REALTIME, the clock
    // pthread_cond_timedwait measures against.
    na_thread_wait_begin(timeout ? "futex(t)" : "futex", (const void *)addr, val);
    int timed_out = 0;
    while (1) {
        uint64_t now_us = sceKernelGetProcessTimeWide();
        if (deadline && now_us >= deadline) {
            timed_out = 1;
            break;
        }
        uint64_t slice = 4000;
        if (deadline && deadline - now_us < slice) slice = deadline - now_us;
        struct timespec abs;
        clock_gettime(CLOCK_REALTIME, &abs);
        abs.tv_nsec += (long)(slice % 1000000) * 1000;
        abs.tv_sec += slice / 1000000 + abs.tv_nsec / 1000000000L;
        abs.tv_nsec %= 1000000000L;
        int r = pthread_cond_timedwait(futex_bucket(addr), &futex_lock, &abs);
        if (r != ETIMEDOUT) break;   // woken
        if (*addr != val) break;     // changed without a wake we saw
    }
    na_thread_wait_end();
    pthread_mutex_unlock(&futex_lock);
    if (timed_out) {
        errno = ETIMEDOUT;
        return -1;
    }
    return 0;
}

static long syscall_na(long n, long a1, long a2, long a3, long a4, long a5) {
    switch (n) {
        case NR_FUTEX:
            return futex_na((volatile int *)a1, (int)a2, (int)a3, (const struct timespec *)a4);
        case NR_GETTID:
            return sceKernelGetThreadId();
        case NR_SCHED_SETAFFINITY:
            return 0;
        case NR_SCHED_GETAFFINITY:
            // three cores
            if (a2 >= 4 && a3) {
                memset((void *)a3, 0, a2);
                *(uint32_t *)a3 = 0x7;
                return 4;
            }
            errno = EINVAL;
            return -1;
        default: {
            static int warned = 0;
            if (warned++ < 32) l_warn("syscall(%ld): not implemented", n);
            errno = ENOSYS;
            return -1;
        }
    }
}

// fnmatch with '*', '?' and '[...]'; flags are ignored (newlib on the Vita has none)
static int fnmatch_na(const char *p, const char *s, int flags) {
    for (; *p; p++, s++) {
        if (*p == '*') {
            while (*p == '*') p++;
            if (!*p) return 0;
            for (; *s; s++)
                if (fnmatch_na(p, s, flags) == 0) return 0;
            return 1;
        }
        if (!*s) return 1;
        if (*p == '[') {
            const char *q = p + 1;
            int neg = (*q == '!' || *q == '^');
            if (neg) q++;
            int ok = 0;
            for (; *q && *q != ']'; q++) {
                if (q[1] == '-' && q[2] && q[2] != ']') {
                    if (*s >= q[0] && *s <= q[2]) ok = 1;
                    q += 2;
                } else if (*q == *s) {
                    ok = 1;
                }
            }
            if (ok == neg || !*q) return 1;
            p = q;
            continue;
        }
        if (*p != '?' && *p != *s) return 1;
    }
    return *s ? 1 : 0;
}

/*
 * What Unity 2020.3 imports and Unity 6 did not (the port started from a
 * Unity 6 game). Things a game on the Vita has no use for (processes,
 * capabilities, CPU affinity, named semaphores) fail or do nothing.
 */
static long pread_na(int fd, void *buf, size_t n, long off) {
    long pos = (long)lseek(fd, 0, SEEK_CUR);
    if (pos < 0 || lseek(fd, off, SEEK_SET) < 0) return -1;
    long got = 0;
    while ((size_t)got < n) {
        long r = read(fd, (char *)buf + got, n - got);
        if (r < 0) {
            if (!got) got = -1;
            break;
        }
        if (r == 0) break;
        got += r;
    }
    lseek(fd, pos, SEEK_SET);
    return got;
}

struct iovec_na {
    void *base;
    size_t len;
};
static long writev_na(int fd, const struct iovec_na *iov, int n) {
    long total = 0;
    for (int i = 0; i < n; i++) {
        if (!iov[i].len) continue;
        long r = write_na(fd, iov[i].base, iov[i].len);
        if (r < 0) return total ? total : -1;
        total += r;
        if ((size_t)r < iov[i].len) break;
    }
    return total;
}

// Relative timeout on a condition variable: made absolute (realtime) for newlib
static int pthread_cond_timedwait_relative_np_na(pthread_cond_t_bionic *cond, pthread_mutex_t_bionic *mutex,
                                                 const struct timespec *rel) {
    if (!rel) return pthread_cond_timedwait_soloader(cond, mutex, NULL);
    struct timespec now;
    clock_gettime_soloader(0 /* CLOCK_REALTIME */, &now);
    int64_t t = (int64_t)now.tv_sec * 1000000000LL + now.tv_nsec + (int64_t)rel->tv_sec * 1000000000LL + rel->tv_nsec;
    struct timespec abs = { .tv_sec = t / 1000000000LL, .tv_nsec = t % 1000000000LL };
    return pthread_cond_timedwait_soloader(cond, mutex, &abs);
}

static int rwlock_destroy_na(void *lock) {
    pthread_rwlock_t **slot = (pthread_rwlock_t **)lock;
    if (slot && *slot) {
        pthread_rwlock_destroy(*slot);
        free(*slot);
        *slot = NULL;
    }
    return 0;
}

static int pthread_attr_getdetachstate_na(const void *attr, int *state) {
    if (state) *state = 0; // PTHREAD_CREATE_JOINABLE
    return 0;
}

static int pthread_getcpuclockid_na(void *thread, int *clock) {
    if (clock) *clock = 3; // CLOCK_THREAD_CPUTIME_ID
    return 0;
}

// One core set: all three cores a game gets
static int sched_getaffinity_na(int pid, size_t size, void *mask) {
    if (mask && size) {
        memset(mask, 0, size);
        *(uint8_t *)mask = 0x7;
    }
    return 0;
}

static void cleanup_push_na(void *c, void (*routine)(void *), void *arg) {}
static void cleanup_pop_na(void *c, int execute) {}

typedef struct {
    long long quot, rem;
} lldiv_na_t;
static lldiv_na_t lldiv_na(long long a, long long b) {
    lldiv_na_t r = { a / b, a % b };
    return r;
}

static char *dirname_na(char *path) {
    static char dot[] = ".";
    if (!path || !*path) return dot;
    char *end = path + strlen(path) - 1;
    while (end > path && *end == '/') *end-- = 0;
    char *slash = strrchr(path, '/');
    if (!slash) return dot;
    if (slash == path) {
        path[1] = 0;
        return path;
    }
    *slash = 0;
    return path;
}

static void *ret_null_na(void) {
    errno = ENOSYS;
    return NULL;
}

static char *environ_empty[] = { NULL };
static char **environ_na = environ_empty; // the symbol is the variable `char **environ`

int na_mprotect(void *addr, size_t length, int prot); // reimpl/mem.c
int na_madvise(void *addr, size_t length, int advice); // reimpl/mem.c

so_default_dynlib na_dynlib[] = {
    // stdio
    { "__sF", (uintptr_t)&sF_bionic },
    { "stdin", (uintptr_t)&stdin_bionic },
    { "stdout", (uintptr_t)&stdout_bionic },
    { "stderr", (uintptr_t)&stderr_bionic },
    { "fprintf", (uintptr_t)&fprintf_na },
    { "vfprintf", (uintptr_t)&vfprintf_na },
    { "printf", (uintptr_t)&printf_na },
    { "vprintf", (uintptr_t)&vprintf_na },
    { "fwrite", (uintptr_t)&fwrite_na },
    { "fputs", (uintptr_t)&fputs_na },
    { "fputc", (uintptr_t)&fputc_na },
    { "puts", (uintptr_t)&puts_na },
    { "fflush", (uintptr_t)&fflush_na },
    { "fileno", (uintptr_t)&fileno_na },
    { "ferror", (uintptr_t)&ferror_na },
    { "feof", (uintptr_t)&feof_na },
    { "clearerr", (uintptr_t)&clearerr_na },
    { "setbuf", (uintptr_t)&setbuf_na },

    // libdl
    { "dlopen", (uintptr_t)&na_dlopen },
    { "dlsym", (uintptr_t)&na_dlsym },
    { "dlclose", (uintptr_t)&na_dlclose },
    { "dlerror", (uintptr_t)&na_dlerror },
    { "dladdr", (uintptr_t)&na_dladdr },
    { "dl_unwind_find_exidx", (uintptr_t)&na_dl_unwind_find_exidx },
    { "dl_iterate_phdr", (uintptr_t)&na_dl_iterate_phdr },

    // Unity 2020.3 (see above)
    { "pread", (uintptr_t)&pread_na },
    { "writev", (uintptr_t)&writev_na },
    { "pthread_cond_timedwait_relative_np", (uintptr_t)&pthread_cond_timedwait_relative_np_na },
    { "pthread_rwlock_destroy", (uintptr_t)&rwlock_destroy_na },
    { "pthread_attr_getdetachstate", (uintptr_t)&pthread_attr_getdetachstate_na },
    { "pthread_getcpuclockid", (uintptr_t)&pthread_getcpuclockid_na },
    { "__pthread_cleanup_push", (uintptr_t)&cleanup_push_na },
    { "__pthread_cleanup_pop", (uintptr_t)&cleanup_pop_na },
    { "sched_getaffinity", (uintptr_t)&sched_getaffinity_na },
    { "sched_setaffinity", (uintptr_t)&ret0 },
    { "lldiv", (uintptr_t)&lldiv_na },
    { "nearbyintf", (uintptr_t)&nearbyintf },
    { "remainderf", (uintptr_t)&remainderf },
    { "asctime", (uintptr_t)&asctime },
    { "dirname", (uintptr_t)&dirname_na },
    { "environ", (uintptr_t)&environ_na },
    { "msync", (uintptr_t)&ret0 },
    { "__system_property_set", (uintptr_t)&ret0 },
    { "capset", (uintptr_t)&fail_enosys },
    { "execl", (uintptr_t)&fail_enosys },
    { "fchown", (uintptr_t)&ret0 },
    { "kill", (uintptr_t)&fail_enosys },
    { "mkdtemp", (uintptr_t)&ret_null_na },
    { "nftw", (uintptr_t)&fail_enosys },
    { "sem_open", (uintptr_t)&ret_null_na },
    { "socketpair", (uintptr_t)&fail_enosys },
    { "popen", (uintptr_t)&ret_null_na },
    { "pclose", (uintptr_t)&fail_enosys },

    // bionic internals
    { "__system_property_get", (uintptr_t)&system_property_get },
    { "__system_property_find", (uintptr_t)&system_property_find },
    { "__system_property_read", (uintptr_t)&system_property_read },
    { "__FD_ISSET_chk", (uintptr_t)&fd_isset_chk },
    { "__FD_SET_chk", (uintptr_t)&fd_set_chk },
    { "__ctype_get_mb_cur_max", (uintptr_t)&ctype_get_mb_cur_max },
    { "__gnu_strerror_r", (uintptr_t)&gnu_strerror_r },
    { "__register_atfork", (uintptr_t)&ret0 },
    { "__end__", 0 },
    { "_ZTH15gDeferredAction", 0 },
    { "android_set_abort_message", (uintptr_t)&android_set_abort_message_na },
    { "getauxval", (uintptr_t)&getauxval_na },
    { "syscall", (uintptr_t)&syscall_na },
    { "sysconf", (uintptr_t)&sysconf_na },

    // no network (see getaddrinfo_na)
    { "getaddrinfo", (uintptr_t)&getaddrinfo_na },
    { "freeaddrinfo", (uintptr_t)&freeaddrinfo_na },
    { "gethostbyname", (uintptr_t)&gethostbyname_na },
    { "socket", (uintptr_t)&socket_na },
    { "connect", (uintptr_t)&connect_na },

    // files (paths inside the APK are mapped, see na_apk.c)
    { "access", (uintptr_t)&access_soloader },
    { "lstat", (uintptr_t)&stat_soloader },
    { "lseek64", (uintptr_t)&lseek64_na },
    { "lseek", (uintptr_t)&lseek_na },
    { "statfs", (uintptr_t)&statfs_na },
    { "readlink", (uintptr_t)&readlink_na },
    { "realpath", (uintptr_t)&realpath_na },
    { "link", (uintptr_t)&fail_enosys },
    { "symlink", (uintptr_t)&fail_enosys },
    { "fchmod", (uintptr_t)&ret0 },
    { "flock", (uintptr_t)&ret0 },
    { "futimens", (uintptr_t)&ret0 },
    { "utimes", (uintptr_t)&ret0 },
    { "sendfile", (uintptr_t)&sendfile_na },
    { "read", (uintptr_t)&read_na },
    { "write", (uintptr_t)&write_na },
    { "rename", (uintptr_t)&rename_na },
    { "mkdir", (uintptr_t)&mkdir_na },
    { "unlink", (uintptr_t)&unlink_na },
    { "rmdir", (uintptr_t)&rmdir_na },
    { "ftruncate", (uintptr_t)&ftruncate_na },
    { "truncate", (uintptr_t)&truncate_na },
    { "dup2", (uintptr_t)&fail_enosys },
    { "isatty", (uintptr_t)&isatty_na },
    { "fnmatch", (uintptr_t)&fnmatch_na },

    // users / processes
    { "getuid", (uintptr_t)&get_uid },
    { "geteuid", (uintptr_t)&get_uid },
    { "getegid", (uintptr_t)&get_uid },
    { "getpid", (uintptr_t)&get_pid },
    { "gettid", (uintptr_t)&get_tid },
    { "getpwuid", (uintptr_t)&getpwuid_na },
    { "getpwuid_r", (uintptr_t)&getpwuid_r_na },
    { "getpriority", (uintptr_t)&ret0 },
    { "setpriority", (uintptr_t)&ret0 },
    { "process_vm_readv", (uintptr_t)&fail_enosys },
    { "ptrace", (uintptr_t)&fail_enosys },
    { "unsetenv", (uintptr_t)&unsetenv_na },
    { "getenv", (uintptr_t)&getenv_na },
    { "setenv", (uintptr_t)&setenv_na },

    // threads
    { "pthread_getattr_np", (uintptr_t)&pthread_getattr_np_na },
    { "pthread_attr_getstack", (uintptr_t)&pthread_attr_getstack_na },
    { "pthread_condattr_init", (uintptr_t)&pthread_condattr_init_soloader },
    { "pthread_condattr_destroy", (uintptr_t)&pthread_condattr_destroy_soloader },
    { "pthread_condattr_setclock", (uintptr_t)&pthread_condattr_setclock_na },
    { "pthread_cond_timedwait", (uintptr_t)&pthread_cond_timedwait_na },
    { "pthread_rwlock_init", (uintptr_t)&rwlock_init_na },
    { "pthread_rwlock_rdlock", (uintptr_t)&rwlock_rdlock_na },
    { "pthread_rwlock_wrlock", (uintptr_t)&rwlock_wrlock_na },
    { "pthread_rwlock_unlock", (uintptr_t)&rwlock_unlock_na },
    { "pthread_kill", (uintptr_t)&pthread_kill_na },

    // signals
    { "sigaction", (uintptr_t)&sigaction_na },
    { "signal", (uintptr_t)&signal_na },
    { "sigemptyset", (uintptr_t)&sigemptyset_na },
    { "sigfillset", (uintptr_t)&sigfillset_na },
    { "sigaddset", (uintptr_t)&sigaddset_na },
    { "sigdelset", (uintptr_t)&sigdelset_na },
    { "sigsuspend", (uintptr_t)&sigsuspend_na },
    { "sigprocmask", (uintptr_t)&sigprocmask_na },
    { "pthread_sigmask", (uintptr_t)&sigprocmask_na },
    { "sigaltstack", (uintptr_t)&ret0 },
    { "sigsetjmp", (uintptr_t)&setjmp },
    { "siglongjmp", (uintptr_t)&longjmp },

    // memory
    { "posix_memalign", (uintptr_t)&posix_memalign_na },
    { "madvise", (uintptr_t)&na_madvise },
    { "mprotect", (uintptr_t)&na_mprotect },

    // math
    { "cbrtf", (uintptr_t)&cbrtf },
    { "hypot", (uintptr_t)&hypot },
    { "hypotf", (uintptr_t)&hypotf },
    { "log2f", (uintptr_t)&log2f },
    { "logb", (uintptr_t)&logb },
    { "modff", (uintptr_t)&modff },
    { "tanhf", (uintptr_t)&tanhf },
    { "div", (uintptr_t)&div_na },

    // locale variants
    { "iswalpha_l", (uintptr_t)&iswalpha_l_na },
    { "iswblank_l", (uintptr_t)&iswblank_l_na },
    { "iswcntrl_l", (uintptr_t)&iswcntrl_l_na },
    { "iswdigit_l", (uintptr_t)&iswdigit_l_na },
    { "iswlower_l", (uintptr_t)&iswlower_l_na },
    { "iswprint_l", (uintptr_t)&iswprint_l_na },
    { "iswpunct_l", (uintptr_t)&iswpunct_l_na },
    { "iswspace_l", (uintptr_t)&iswspace_l_na },
    { "iswupper_l", (uintptr_t)&iswupper_l_na },
    { "iswxdigit_l", (uintptr_t)&iswxdigit_l_na },
    { "towlower_l", (uintptr_t)&towlower_l_na },
    { "towupper_l", (uintptr_t)&towupper_l_na },
    { "strcoll_l", (uintptr_t)&strcoll_l_na },
    { "wcscoll_l", (uintptr_t)&wcscoll_l_na },
    { "strxfrm_l", (uintptr_t)&strxfrm_l_na },
    { "wcsxfrm_l", (uintptr_t)&wcsxfrm_l_na },
    { "strftime_l", (uintptr_t)&strftime_l_na },
    { "strtold_l", (uintptr_t)&strtold_l_na },
    { "strtoll_l", (uintptr_t)&strtoll_l_na },
    { "strtoull_l", (uintptr_t)&strtoull_l_na },
    { "wcstold", (uintptr_t)&wcstold_na },

    // logging / network
    { "openlog", (uintptr_t)&ret0 },
    { "closelog", (uintptr_t)&ret0 },
    { "syslog", (uintptr_t)&syslog_na },
    { "getnameinfo", (uintptr_t)&fail_enosys },
    { "if_nametoindex", (uintptr_t)&if_nametoindex_na },
    { "inet_addr", (uintptr_t)&inet_addr_na },

    // OpenSL ES interface ids FMOD asks for (na_sles.c refuses them)
    { "SL_IID_RECORD", (uintptr_t)&SL_IID_RECORD },
    { "SL_IID_ANDROIDCONFIGURATION", (uintptr_t)&SL_IID_ANDROIDCONFIGURATION },

    // EGL additions
    { "eglSurfaceAttrib", (uintptr_t)&na_eglSurfaceAttrib },

    // NDK (na_android.c)
    NA_ANDROID_DYNLIB_ENTRIES
};

const int na_dynlib_count = sizeof(na_dynlib) / sizeof(na_dynlib[0]);

void na_dynlib_init(void) {
    setenv_na("GC_DONT_GC", "1", 1);
    setenv_na("HOME", DATA_PATH "files", 1);
    setenv_na("TMPDIR", DATA_PATH "cache", 1);
    setenv_na("EXTERNAL_STORAGE", DATA_PATH "external", 1);
    setenv_na("ANDROID_ROOT", "/system", 1);
    setenv_na("ANDROID_DATA", "/data", 1);
    for (int i = 0; i < FUTEX_BUCKETS; i++)
        pthread_cond_init(&futex_cond[i], NULL);
    futex_ready = 1;
    sceKernelCreateLwMutex(&rwlock_init_lock, "rwlock_init", 0, 0, NULL);
    rwlock_init_lock_ready = 1;
}
