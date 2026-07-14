#ifndef PIPELINE_H
#define PIPELINE_H

#include "rdp_commands.h"
#include "combiner.h"
#include "raster_coverage.h"

typedef rdp_combiner_inputs pipeline_inputs;

typedef struct pipeline_outputs {
    rdp_color color;
    uint8_t coverage;
} pipeline_outputs;

typedef struct rdp_fragment {
    rdp_color color;
    uint16_t alpha;
    uint8_t coverage;
    uint8_t shade_alpha;
} rdp_fragment;

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
    uint8_t texture_coord_shift;
    raster_shade_setup shade;
    raster_coverage_span coverage;
} rdp_span_work;

typedef enum rdp_span_kernel_kind {
    RDP_SPAN_KERNEL_INVALID = 0,
    RDP_SPAN_KERNEL_TRIANGLE,
    RDP_SPAN_KERNEL_TEXTURE_RECTANGLE,
    RDP_SPAN_KERNEL_TEXTURE_RECTANGLE_COPY
} rdp_span_kernel_kind;

typedef enum rdp_block_stage {
    RDP_BLOCK_STAGE_TEXTURE       = 1u << 0,
    RDP_BLOCK_STAGE_DEPTH         = 1u << 1,
    RDP_BLOCK_STAGE_BLEND         = 1u << 2,
    RDP_BLOCK_STAGE_ALPHA_COMPARE = 1u << 3,
    RDP_BLOCK_STAGE_SHADE         = 1u << 4,
    RDP_BLOCK_STAGE_FILL          = 1u << 5,
    RDP_BLOCK_STAGE_LOD           = 1u << 6
} rdp_block_stage;

typedef enum rdp_block_sampler_kind {
    RDP_BLOCK_SAMPLER_NONE = 0,
    RDP_BLOCK_SAMPLER_GENERIC,
    RDP_BLOCK_SAMPLER_RGBA16_POINT,
    RDP_BLOCK_SAMPLER_RGBA16_BILERP,
    RDP_BLOCK_SAMPLER_I4_BILERP,
    RDP_BLOCK_SAMPLER_CI8_TLUT_BILERP,
    RDP_BLOCK_SAMPLER_I8_BILERP,
    RDP_BLOCK_SAMPLER_IA8_BILERP
} rdp_block_sampler_kind;

typedef enum rdp_block_coord_kind {
    RDP_BLOCK_COORD_DIRECT = 0,
    RDP_BLOCK_COORD_PERSPECTIVE
} rdp_block_coord_kind;

typedef enum rdp_block_depth_kind {
    RDP_BLOCK_DEPTH_NONE = 0,
    RDP_BLOCK_DEPTH_TEST,
    RDP_BLOCK_DEPTH_UPDATE,
    RDP_BLOCK_DEPTH_TEST_UPDATE
} rdp_block_depth_kind;

typedef enum rdp_block_framebuffer_kind {
    RDP_BLOCK_FRAMEBUFFER_8 = 0,
    RDP_BLOCK_FRAMEBUFFER_16,
    RDP_BLOCK_FRAMEBUFFER_32
} rdp_block_framebuffer_kind;

/* Primitive-constant execution metadata consumed once per fragment block. */
typedef struct rdp_block_plan {
    uint32_t stages;
    rdp_block_sampler_kind sampler;
    rdp_block_coord_kind coordinates;
    rdp_block_depth_kind depth;
    rdp_block_framebuffer_kind framebuffer;
} rdp_block_plan;

typedef struct rdp_primitive_state rdp_primitive_state;

/*
 * A draw-local snapshot of register-derived state. The command processor owns
 * mutable RDP registers; span rendering only sees this immutable value.
 */
struct rdp_primitive_state {
    rdp_framebuffer_state framebuffer;
    rdp_texture_sample_state texture;
    rdp_texture_sample_state texture_cycle1;
    uint8_t lod_base_tile;
    uint8_t lod_max_level;
    uint8_t lod_min_level;
    bool texture_lod;
    bool sharpen_lod;
    bool detail_lod;
    rdp_color_pipeline_state color;
    rdp_fragment_state fragment;
    const tmem_state *tmem;
    raster_decoded_triangle triangle;
    rdp_block_plan block_plan;
    rdp_span_kernel_kind span_kernel;
    bool fill_mode;
    /* Kept last so non-LOD primitives do not clear this large cold storage. */
    rdp_texture_sample_state lod_textures[8];
    rdp_texture_sample_state lod_textures_cycle1[8];
};

pipeline_outputs pipeline_combine_pixel(const rdp_color_pipeline_state *state, const pipeline_inputs *inputs);

void pipeline_compile_framebuffer(rdp_framebuffer_state *framebuffer,
                                  const rdp_state *registers);

void pipeline_compile_triangle(rdp_primitive_state *primitive,
                               const rdp_state *registers,
                               const tmem_state *tmem,
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
                                uint32_t tile_index);
void pipeline_compile_color_rectangle(rdp_primitive_state *primitive,
                                      const rdp_state *registers,
                                      const tmem_state *tmem);

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

sr_result pipeline_render_copy_rectangle_span(sr_memory *memory,
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
    case RDP_SPAN_KERNEL_TRIANGLE:
        return pipeline_render_triangle_span(memory, primitive, work);
    case RDP_SPAN_KERNEL_TEXTURE_RECTANGLE:
        return pipeline_render_rectangle_span(memory, primitive, work);
    case RDP_SPAN_KERNEL_TEXTURE_RECTANGLE_COPY:
        return pipeline_render_copy_rectangle_span(memory, primitive, work);
    default:
        return SR_ERROR_INVALID_ARGUMENT;
    }
}

void pipeline_resolve_tile_bounds(const rdp_state *state, const tmem_state *tmem, uint32_t tile_index, rdp_tile_bounds *bounds);

#endif
