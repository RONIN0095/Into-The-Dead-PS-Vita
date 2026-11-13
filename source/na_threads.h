#ifndef NA_THREADS_H
#define NA_THREADS_H

// Thread report for hangs, see na_threads.c
void na_thread_register(const char *tag, void *start);
void na_thread_wait_begin(const char *what, const void *addr, int val);
void na_thread_wait_end(void);
void na_threads_report(void);

#endif
