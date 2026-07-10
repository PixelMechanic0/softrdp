#ifndef PIPELINE_H
#define PIPELINE_H

#include "rdp_commands.h"
#include "rdp_metrics.h"

typedef struct pipeline_inputs {
    rdp_color shade;
    rdp_color texel0;
    rdp_color texel1;
    rdp_color primitive;
} pipeline_inputs;

typedef struct pipeline_outputs {
    rdp_color color;
    uint8_t coverage;
} pipeline_outputs;

/*
 * A draw-local snapshot of register-derived state. The command processor owns
 * mutable RDP registers; span rendering only sees this immutable value.
 */
typedef struct rdp_primitive_state {
    rdp_framebuffer_state framebuffer;
    rdp_texture_sample_state texture;
    rdp_depth_state depth;
    rdp_color_pipeline_state color;
    const tmem_state *tmem;
    rdp_metrics *metrics;
    raster_decoded_triangle triangle;
    bool fill_mode;
} rdp_primitive_state;

/* Incremental values at the start of one scanline span. */
typedef struct rdp_span_work {
    int x_begin;
    int x_end;
    int y;
    int64_t depth_fixed;
    int32_t s_fixed;
    int32_t t_fixed;
    int32_t w_fixed;
    raster_shade_setup shade;
} rdp_span_work;

pipeline_outputs pipeline_shade_pixel(const rdp_color_pipeline_state *state, const pipeline_inputs *inputs);

void pipeline_compile_framebuffer(rdp_framebuffer_state *framebuffer,
                                  const rdp_state *registers);

void pipeline_compile_triangle(rdp_primitive_state *primitive,
                               const rdp_state *registers,
                               const tmem_state *tmem,
                               rdp_metrics *metrics,
                               const raster_decoded_triangle *triangle,
                               bool fill_mode);

void pipeline_setup_triangle_span(const rdp_primitive_state *primitive,
                                  int x_begin,
                                  int x_end,
                                  int y,
                                  rdp_span_work *work);

sr_result pipeline_render_triangle_span(sr_memory *memory,
                                        const rdp_primitive_state *primitive,
                                        const rdp_span_work *work);

void pipeline_compile_rectangle(rdp_primitive_state *primitive,
                                const rdp_state *registers,
                                const tmem_state *tmem,
                                rdp_metrics *metrics,
                                uint32_t tile_index);

void pipeline_setup_rectangle_span(int x_begin,
                                   int x_end,
                                   int y,
                                   int32_t s_fixed,
                                   int32_t t_fixed,
                                   rdp_span_work *work);

sr_result pipeline_render_rectangle_span(sr_memory *memory,
                                         const rdp_primitive_state *primitive,
                                         const rdp_span_work *work,
                                         int32_t dsdx,
                                         int32_t dtdx);

void pipeline_resolve_tile_bounds(const rdp_state *state, const tmem_state *tmem, uint32_t tile_index, rdp_tile_bounds *bounds);

#endif
