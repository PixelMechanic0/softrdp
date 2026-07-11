#ifndef PIPELINE_H
#define PIPELINE_H

#include "rdp_commands.h"
#include "rdp_metrics.h"
#include "combiner.h"

typedef rdp_combiner_inputs pipeline_inputs;

typedef struct pipeline_outputs {
    rdp_color color;
    uint8_t coverage;
} pipeline_outputs;

/* Incremental values at the start of one scanline span. */
typedef struct rdp_span_work {
    int x_begin;
    int x_end;
    int y;
    int64_t depth_fixed;
    int32_t s_fixed;
    int32_t t_fixed;
    int32_t w_fixed;
    int32_t dsdx_fixed;
    int32_t dtdx_fixed;
    raster_shade_setup shade;
} rdp_span_work;

typedef enum rdp_span_kernel_kind {
    RDP_SPAN_KERNEL_INVALID = 0,
    RDP_SPAN_KERNEL_FILL_TRIANGLE,
    RDP_SPAN_KERNEL_DEPTH_TRIANGLE,
    RDP_SPAN_KERNEL_SHADE_TRIANGLE,
    RDP_SPAN_KERNEL_SHADE_DEPTH_TRIANGLE,
    RDP_SPAN_KERNEL_TEXTURE_TRIANGLE,
    RDP_SPAN_KERNEL_TEXTURE_RECTANGLE
} rdp_span_kernel_kind;

typedef struct rdp_primitive_state rdp_primitive_state;

/*
 * A draw-local snapshot of register-derived state. The command processor owns
 * mutable RDP registers; span rendering only sees this immutable value.
 */
struct rdp_primitive_state {
    rdp_framebuffer_state framebuffer;
    rdp_texture_sample_state texture;
    rdp_color_pipeline_state color;
    rdp_fragment_state fragment;
    const tmem_state *tmem;
    rdp_metrics *metrics;
    raster_decoded_triangle triangle;
    rdp_span_kernel_kind span_kernel;
    bool fill_mode;
};

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

sr_result pipeline_render_fill_triangle_span(sr_memory *memory,
                                             const rdp_primitive_state *primitive,
                                             const rdp_span_work *work);

sr_result pipeline_render_depth_triangle_span(sr_memory *memory,
                                               const rdp_primitive_state *primitive,
                                               const rdp_span_work *work);

sr_result pipeline_render_shade_triangle_span(sr_memory *memory,
                                              const rdp_primitive_state *primitive,
                                              const rdp_span_work *work);

sr_result pipeline_render_shade_depth_triangle_span(sr_memory *memory,
                                                    const rdp_primitive_state *primitive,
                                                    const rdp_span_work *work);

rdp_span_kernel_kind pipeline_select_triangle_kernel(const rdp_primitive_state *primitive);

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
                                   int32_t dsdx_fixed,
                                   int32_t dtdx_fixed,
                                   rdp_span_work *work);

sr_result pipeline_render_rectangle_span(sr_memory *memory,
                                         const rdp_primitive_state *primitive,
                                         const rdp_span_work *work);

static inline sr_result pipeline_render_span(sr_memory *memory,
                                             const rdp_primitive_state *primitive,
                                             const rdp_span_work *work)
{
    if (!memory || !primitive || !work) {
        return SR_ERROR_INVALID_ARGUMENT;
    }

    switch (primitive->span_kernel) {
    case RDP_SPAN_KERNEL_FILL_TRIANGLE:
        return pipeline_render_fill_triangle_span(memory, primitive, work);
    case RDP_SPAN_KERNEL_DEPTH_TRIANGLE:
        return pipeline_render_depth_triangle_span(memory, primitive, work);
    case RDP_SPAN_KERNEL_SHADE_TRIANGLE:
        return pipeline_render_shade_triangle_span(memory, primitive, work);
    case RDP_SPAN_KERNEL_SHADE_DEPTH_TRIANGLE:
        return pipeline_render_shade_depth_triangle_span(memory, primitive, work);
    case RDP_SPAN_KERNEL_TEXTURE_TRIANGLE:
        return pipeline_render_triangle_span(memory, primitive, work);
    case RDP_SPAN_KERNEL_TEXTURE_RECTANGLE:
        return pipeline_render_rectangle_span(memory, primitive, work);
    default:
        return SR_ERROR_INVALID_ARGUMENT;
    }
}

void pipeline_resolve_tile_bounds(const rdp_state *state, const tmem_state *tmem, uint32_t tile_index, rdp_tile_bounds *bounds);

#endif
