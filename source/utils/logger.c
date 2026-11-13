/*
 * Copyright (C) 2022-2024 Volodymyr Atamanenko
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

#include "utils/logger.h"

#include <psp2/io/fcntl.h>
#include <psp2/kernel/clib.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>

#include <stdbool.h>
#include <stdatomic.h>

#define COLOR_RED    "\x1B[38;5;196m"
#define COLOR_PINK   "\x1B[38;5;212m"
#define COLOR_ORANGE "\x1B[38;5;202m"
#define COLOR_BLUE   "\x1B[38;5;32m"
#define COLOR_GREEN  "\x1B[32m"
#define COLOR_CYAN   "\x1B[36m"

#define COLOR_END    "\033[0m"

static SceKernelLwMutexWork _log_mutex;
static atomic_bool _log_mutex_ready = ATOMIC_VAR_INIT(false);

// Buffer A is used to adjust the format string.
static char buffer_a[2048];
// Buffer B is used to compile the final log using the updated format string.
static char buffer_b[2048];
// Buffer C holds the uncolored line for the log file.
static char buffer_c[2048];

#define LOG_FILE DATA_PATH "log.txt"

#define LOG_RECENT_LINES 24
#define LOG_RECENT_LEN 120
static char recent_lines[LOG_RECENT_LINES][LOG_RECENT_LEN];
static int recent_next = 0, recent_count = 0;

static const char *level_names[] = { "debug", "info", "warn", "ERROR", "FATAL", "ok", "wait" };

// The file stays open: opening it per line took up to half a second on a
// memory card. Each line is written with sceIoWrite right away, so it
// survives a crash (the kernel closes the file).
static SceUID log_fd = -1;
static int log_lines = 0;
#define LOG_MAX_LINES 20000

static char wbuf[32 * 1024];
static int wbuf_len = 0;

static void log_to_file(int t, const char *fmt, va_list list) {
    if (log_fd < 0) {
        log_fd = sceIoOpen(LOG_FILE, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0777);
        if (log_fd < 0) return;
    }
    SceUID fd = log_fd;
    if (++log_lines > LOG_MAX_LINES && t != LT_ERROR && t != LT_FATAL) return;

    unsigned int ms = (unsigned int)(sceKernelGetProcessTimeWide() / 1000);
    int n = sceClibSnprintf(buffer_c, sizeof(buffer_c), "%6u.%03u [%s] ", ms / 1000, ms % 1000, level_names[t]);
    n += sceClibVsnprintf(buffer_c + n, sizeof(buffer_c) - n, fmt, list);
    if (n > (int)sizeof(buffer_c) - 2) n = sizeof(buffer_c) - 2;
    buffer_c[n++] = '\n';
    // Buffered: one write per line made loading slow (hundreds of lines while
    // a scene loads). Warnings and worse are written at once; the watchdog
    // flushes the rest every second (log_flush).
    if (wbuf_len + n > (int)sizeof(wbuf)) {
        sceIoWrite(fd, wbuf, wbuf_len);
        wbuf_len = 0;
    }
    sceClibMemcpy(wbuf + wbuf_len, buffer_c, n);
    wbuf_len += n;
    if (t >= LT_WARN && t <= LT_FATAL) {
        sceIoWrite(fd, wbuf, wbuf_len);
        wbuf_len = 0;
    }

    // Keep the last lines for the on-screen hang report
    buffer_c[n - 1] = '\0';
    sceClibStrncpy(recent_lines[recent_next], buffer_c, LOG_RECENT_LEN - 1);
    recent_next = (recent_next + 1) % LOG_RECENT_LINES;
    if (recent_count < LOG_RECENT_LINES) recent_count++;
}

int log_recent_count(void) {
    return recent_count;
}

const char *log_recent_line(int i) {
    int first = (recent_next - recent_count + LOG_RECENT_LINES) % LOG_RECENT_LINES;
    return recent_lines[(first + i) % LOG_RECENT_LINES];
}

// Time of the last log line (for the watchdog in main.c)
volatile uint64_t log_last_activity_us = 0;

void _log_print(int t, const char* fmt, ...) {
    log_last_activity_us = sceKernelGetProcessTimeWide();
    if (!atomic_load_explicit(&_log_mutex_ready, memory_order_relaxed)) {
        int ret = sceKernelCreateLwMutex(&_log_mutex, "log_lock", 0, 0, NULL);
        if (ret < 0) {
            sceClibPrintf("Error: failed to create log mutex: 0x%x\n", ret);
            return;
        }
        atomic_store_explicit(&_log_mutex_ready, true, memory_order_relaxed);
    }
    sceKernelLockLwMutex(&_log_mutex, 1, NULL);

    switch (t) {
        case LT_DEBUG:
            sceClibSnprintf(buffer_a, sizeof(buffer_a), " %s• debug%s    %s\n",
                            COLOR_PINK, COLOR_END, fmt); break;
        case LT_INFO:
            sceClibSnprintf(buffer_a, sizeof(buffer_a), " %sℹ info%s     %s\n",
                            COLOR_BLUE, COLOR_END, fmt); break;
        case LT_WARN:
            sceClibSnprintf(buffer_a, sizeof(buffer_a), " %s⚠ warning%s  %s\n",
                            COLOR_ORANGE, COLOR_END, fmt); break;
        case LT_ERROR:
            sceClibSnprintf(buffer_a, sizeof(buffer_a), " %s⨯ error%s    %s\n",
                            COLOR_RED, COLOR_END, fmt); break;
        case LT_FATAL:
            sceClibSnprintf(buffer_a, sizeof(buffer_a), " %s! fatal%s    %s\n",
                            COLOR_RED, COLOR_END, fmt); break;
        case LT_SUCCESS:
            sceClibSnprintf(buffer_a, sizeof(buffer_a), " %s! success%s  %s\n",
                            COLOR_GREEN, COLOR_END, fmt); break;
        case LT_WAIT:
            sceClibSnprintf(buffer_a, sizeof(buffer_a), " %s… waiting%s  %s\n",
                            COLOR_CYAN, COLOR_END, fmt); break;
        default:
            if (atomic_load_explicit(&_log_mutex_ready, memory_order_relaxed)) {
                sceKernelUnlockLwMutex(&_log_mutex, 1);
            }
            return;
    }

    va_list list;
    va_start(list, fmt);
    sceClibVsnprintf(buffer_b, sizeof(buffer_b), buffer_a, list);
    va_end(list);
    sceClibPrintf(buffer_b);

    va_start(list, fmt);
    log_to_file(t, fmt, list);
    va_end(list);

    if (atomic_load_explicit(&_log_mutex_ready, memory_order_relaxed)) {
        sceKernelUnlockLwMutex(&_log_mutex, 1);
    }
}

// Appends raw text (no timestamp, no newline) to the log file.
int log_raw_printf(const char *fmt, ...) {
    static char buf[512];
    va_list list;
    va_start(list, fmt);
    int n = sceClibVsnprintf(buf, sizeof(buf), fmt, list);
    va_end(list);
    if (n > (int)sizeof(buf) - 1) n = sizeof(buf) - 1;
    if (log_fd < 0) log_fd = sceIoOpen(LOG_FILE, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0777);
    if (log_fd < 0) return 0;
    sceIoWrite(log_fd, buf, n);
    return n;
}

// Writes buffered log lines to the file (the watchdog calls it every second)
void log_flush(void) {
    if (!atomic_load_explicit(&_log_mutex_ready, memory_order_relaxed)) return;
    sceKernelLockLwMutex(&_log_mutex, 1, NULL);
    if (log_fd >= 0 && wbuf_len > 0) {
        sceIoWrite(log_fd, wbuf, wbuf_len);
        wbuf_len = 0;
    }
    sceKernelUnlockLwMutex(&_log_mutex, 1);
}
