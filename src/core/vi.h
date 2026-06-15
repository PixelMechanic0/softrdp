#ifndef VI_H
#define VI_H

#include "sr_host.h"

typedef struct sr_memory sr_memory;

typedef struct vi_state {
    uint32_t control;
    uint32_t origin;
    uint32_t width;
    uint32_t x_scale;
    uint32_t y_scale;
} vi_state;

void vi_init(vi_state *vi);
void vi_latch_registers(vi_state *vi, const sr_host_interface *host);
sr_result vi_scanout(const vi_state *vi, const sr_memory *memory, sr_framebuffer *out);

#endif
