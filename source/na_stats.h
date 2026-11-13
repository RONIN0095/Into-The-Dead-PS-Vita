#ifndef NA_STATS_H
#define NA_STATS_H
#include <stdint.h>
// Per-frame counters for the hitch log (main.c): what a slow frame spent its time on
typedef struct {
    uint32_t tex_us, tex_n;     // texture uploads
    uint32_t link_us, link_n;   // shader program links
    uint32_t read_us, read_kb;  // file reads
    uint32_t swap_us;           // eglSwapBuffers
    uint32_t finish_us;         // fence waits (glFinish)
} na_frame_stats;
extern volatile na_frame_stats na_stats;
#endif
