#ifndef NA_TRACE_H
#define NA_TRACE_H

// Where the main loop is: 1 Unity start, 2 input, 3 looper, 4 Java tick,
// 5 Unity frame, 6 file cache, 7 HUD (see main.c)
extern volatile int na_main_phase;

void na_trace_start(void);
void na_trace_report(void);
// A function pointer the main thread's calls through are recorded for (GL
// entry points handed to Unity); returns `fn` itself if it can't.
void *na_trace_wrap(const char *name, void *fn);

#endif
