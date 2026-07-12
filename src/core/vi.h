#ifndef VI_H
#define VI_H

#include "sr_host.h"

typedef struct sr_memory sr_memory;

#define VI_MAX_OUTPUT_WIDTH 640u
#define VI_MAX_OUTPUT_HEIGHT 576u
#define VI_MAX_SOURCE_WIDTH 1024u

typedef struct vi_state {
    uint32_t control;
    uint32_t origin;
    uint32_t width;
    uint32_t current;
    uint32_t v_sync;
    uint32_t h_start;
    uint32_t v_start;
    uint32_t x_scale;
    uint32_t y_scale;
} vi_state;

typedef enum vi_scanout_state {
    VI_SCANOUT_BLANK = 0,
    VI_SCANOUT_READY,
    VI_SCANOUT_INVALID_MEMORY
} vi_scanout_state;

typedef struct vi_x_sample {
    uint16_t source_x;
    uint8_t fraction;
} vi_x_sample;

typedef struct vi_y_sample {
    uint16_t source_y;
    uint8_t fraction;
} vi_y_sample;

typedef struct vi_scanout_plan {
    vi_scanout_state state;
    uint32_t origin;
    uint32_t source_stride;
    uint32_t bytes_per_pixel;
    uint32_t output_width;
    uint32_t output_height;
    uint32_t display_width;
    uint32_t display_height;
    uint32_t aa_mode;
    bool serrate;
    bool gamma_enable;
    bool gamma_dither_enable;
    bool divot_enable;
    bool dither_filter_enable;
    vi_x_sample x_samples[VI_MAX_OUTPUT_WIDTH];
    vi_y_sample y_samples[VI_MAX_OUTPUT_HEIGHT];
} vi_scanout_plan;

void vi_init(vi_state *vi);
void vi_latch_registers(vi_state *vi, const sr_host_interface *host);
void vi_build_scanout_plan(const vi_state *vi, const sr_memory *memory,
                           vi_scanout_plan *plan);
sr_result vi_execute_scanout(const vi_scanout_plan *plan,
                             const sr_memory *memory,
                             sr_framebuffer *out);

#endif
