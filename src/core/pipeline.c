#include "pipeline.h"
#include <string.h>
#include "framebuffer.h"
#include "rdp_memory.h"
#include "tmem.h"
#include "blender.h"

pipeline_outputs pipeline_shade_pixel(const rdp_color_pipeline_state *state, const pipeline_inputs *inputs)
{
    pipeline_outputs out = { .coverage = 7 };
    out.color = (!state || !inputs)
        ? (rdp_color){0, 0, 0, 255}
        : rdp_combiner_evaluate(&state->program, state->cycle_type, inputs);
    return out;
}

/* Moved Fragment Processing Helper Functions */

static uint8_t shade_component_to_u8(int32_t value)
{
    int32_t component = value >> 16;
    return component < 0 ? 0 : (component > 255 ? 255 : (uint8_t)component);
}

static inline rdp_color shade_base_color(const raster_shade_setup *shade)
{
    return (rdp_color){
        shade_component_to_u8(shade->r),
        shade_component_to_u8(shade->g),
        shade_component_to_u8(shade->b),
        shade_component_to_u8(shade->a)
    };
}

static inline rdp_color combine_shade_pixel(const rdp_primitive_state *primitive,
                                            const raster_shade_setup *shade)
{
    const pipeline_inputs inputs = {
        .shade = shade_base_color(shade),
        .primitive = primitive->color.primitive_color,
        .environment = primitive->color.environment_color,
        .primitive_lod_fraction = primitive->color.primitive_lod_fraction
    };
    return pipeline_shade_pixel(&primitive->color, &inputs).color;
}

static int32_t triangle_coord_fixed5(int32_t interpolated)
{
    return (int16_t)((uint32_t)interpolated >> 16);
}

static int32_t perspective_divide_coord(int32_t coord, int32_t w)
{
    const int32_t coord_fixed5 = triangle_coord_fixed5(coord);
    const int32_t w_fixed15 = (int16_t)((uint32_t)w >> 16);
    if (w_fixed15 <= 0) {
        return 0x7fffu;
    }
    const int64_t divided = ((int64_t)coord_fixed5 * 32768ll) / w_fixed15;
    return divided < -0x10000ll ? -0x10000 : (divided > 0xffffll ? 0xffff : (int32_t)divided);
}

static uint32_t texture_coord_to_texel(int32_t value)
{
    return value <= 0 ? 0 : (uint32_t)value >> 5;
}

static void record_texture_sample_coord(rdp_metrics *metrics, uint32_t s, uint32_t t)
{
    if (!metrics) {
        return;
    }

    if (metrics->texture_sample_attempts == 0) {
        metrics->texture_sample_min_s = s;
        metrics->texture_sample_max_s = s;
        metrics->texture_sample_min_t = t;
        metrics->texture_sample_max_t = t;
    } else {
        if (s < metrics->texture_sample_min_s) metrics->texture_sample_min_s = s;
        if (s > metrics->texture_sample_max_s) metrics->texture_sample_max_s = s;
        if (t < metrics->texture_sample_min_t) metrics->texture_sample_min_t = t;
        if (t > metrics->texture_sample_max_t) metrics->texture_sample_max_t = t;
    }
}

static void record_texture_sample_fixed(rdp_metrics *metrics, int32_t s, int32_t t, int32_t w)
{
    if (!metrics) {
        return;
    }

    if (metrics->texture_sample_attempts == 0) {
        metrics->texture_sample_min_s_fixed = s;
        metrics->texture_sample_max_s_fixed = s;
        metrics->texture_sample_min_t_fixed = t;
        metrics->texture_sample_max_t_fixed = t;
        metrics->texture_sample_min_w_fixed = w;
        metrics->texture_sample_max_w_fixed = w;
    } else {
        if (s < metrics->texture_sample_min_s_fixed) metrics->texture_sample_min_s_fixed = s;
        if (s > metrics->texture_sample_max_s_fixed) metrics->texture_sample_max_s_fixed = s;
        if (t < metrics->texture_sample_min_t_fixed) metrics->texture_sample_min_t_fixed = t;
        if (t > metrics->texture_sample_max_t_fixed) metrics->texture_sample_max_t_fixed = t;
        if (w < metrics->texture_sample_min_w_fixed) metrics->texture_sample_min_w_fixed = w;
        if (w > metrics->texture_sample_max_w_fixed) metrics->texture_sample_max_w_fixed = w;
    }
}

static void record_texture_sample_color(rdp_metrics *metrics, rdp_color color)
{
    if (!metrics) {
        return;
    }

    const uint32_t packed = ((uint32_t)color.r << 24) |
                            ((uint32_t)color.g << 16) |
                            ((uint32_t)color.b << 8) |
                            (uint32_t)color.a;
    metrics->texture_sample_color_xor ^= packed;
}

static void record_texture_sample_attempt(rdp_metrics *metrics,
                                          const rdp_texture_sample_state *texture,
                                          const rdp_color_pipeline_state *color)
{
    if (!metrics || !texture || !color) {
        return;
    }

    const rdp_tile *tile = &texture->tile;
    metrics->texture_sample_attempts++;
    if (tile->format <= RDP_FORMAT_I && tile->size <= RDP_SIZE_32BPP) {
        metrics->texture_sample_by_format_size[tile->format][tile->size]++;
    }
    if (texture->tlut_enable) {
        metrics->texture_sample_tlut_attempts++;
    }
    if (texture->bilerp) {
        metrics->texture_sample_bilerp_attempts++;
    }
    if (texture->sample_quad) {
        metrics->texture_sample_quad_attempts++;
    }
    if (texture->mid_texel) {
        metrics->texture_sample_mid_texel_attempts++;
    }
    if (texture->perspective) {
        metrics->texture_sample_perspective_attempts++;
    }
    if (color->needs_texel0 && color->needs_shade) {
        metrics->texture_sample_texel0_shade_attempts++;
    }
}

static void record_texture_sample_hit(rdp_metrics *metrics, const rdp_tile *tile)
{
    if (!metrics || !tile) {
        return;
    }

    metrics->texture_sample_hits++;
    if (tile->format <= RDP_FORMAT_I && tile->size <= RDP_SIZE_32BPP) {
        metrics->texture_sample_hits_by_format_size[tile->format][tile->size]++;
    }
}

static sr_result pipeline_write_fragment(sr_memory *memory,
                                         const rdp_primitive_state *primitive,
                                         uint32_t x, uint32_t y,
                                         rdp_color pixel,
                                         uint8_t shade_alpha,
                                         bool *accepted)
{
    if (accepted) *accepted = false;
    const rdp_fragment_state *state = &primitive->fragment;
    rdp_fragment fragment = {
        .color = pixel,
        .alpha = pixel.a,
        .coverage = 8u,
        .shade_alpha = shade_alpha
    };
    if (state->cvg_times_alpha) {
        fragment.alpha = (uint16_t)(((uint32_t)fragment.alpha * fragment.coverage + 4u) >> 3);
        fragment.coverage = (uint8_t)((fragment.alpha >> 5) & 0xfu);
    }
    if (state->alpha_cvg_select && !state->cvg_times_alpha)
        fragment.alpha = (uint16_t)fragment.coverage << 5;

    uint8_t alpha_threshold = state->blend.blend_color.a;
    if (state->alpha_compare_dither)
        alpha_threshold = (uint8_t)((x * 17u + y * 131u + x * y * 3u) & 0xffu);
    if (state->blend.alpha_compare && fragment.alpha < alpha_threshold)
        return SR_OK;

    rdp_memory_pixel memory_pixel;
    const sr_result read = framebuffer_read_memory_pixel(memory, &primitive->framebuffer,
                                                         x, y, state->blend.image_read,
                                                         &memory_pixel);
    if (read != SR_OK) return read;

    const bool coverage_overflow = ((fragment.coverage + memory_pixel.coverage) & 8u) != 0u;
    const bool blend_enable = state->blend.force_blend ||
                              (state->antialias && !coverage_overflow);
    fragment.color = rdp_blender_evaluate(&state->blend, fragment.color,
                                          fragment.alpha, memory_pixel.color,
                                          fragment.shade_alpha, blend_enable);

    uint32_t final_coverage;
    switch (state->coverage_dest & 3u) {
    case 0:
        final_coverage = blend_enable ? fragment.coverage + memory_pixel.coverage
                                      : fragment.coverage - 1u;
        if (final_coverage > 7u) final_coverage = 7u;
        break;
    case 1: final_coverage = (fragment.coverage + memory_pixel.coverage) & 7u; break;
    case 2: final_coverage = 7u; break;
    default:final_coverage = memory_pixel.coverage; break;
    }
    fragment.color.a = (uint8_t)((final_coverage << 5) | 0x1fu);
    const sr_result result = framebuffer_write_color(memory, &primitive->framebuffer,
                                                     x, y, fragment.color);
    if (result == SR_OK && accepted) *accepted = true;
    return result;
}

typedef struct rdp_depth_result {
    bool pass;
    bool update;
    uint32_t address;
    uint16_t compressed;
} rdp_depth_result;

static inline void triangle_span_work_step(const raster_decoded_triangle *decoded,
                                           const rdp_primitive_state *primitive,
                                           rdp_span_work *work)
{
    if (decoded->has_depth) {
        work->depth_fixed += decoded->depth.dzdx;
    }
    if (decoded->has_texture && primitive->color.needs_texel0) {
        work->s_fixed += decoded->texture.dsdx;
        work->t_fixed += decoded->texture.dtdx;
        if (primitive->texture.perspective) {
            work->w_fixed += decoded->texture.dwdx;
        }
    }
    if (decoded->has_shade) {
        work->shade.r += decoded->shade.drdx & ~0x1f;
        work->shade.g += decoded->shade.dgdx & ~0x1f;
        work->shade.b += decoded->shade.dbdx & ~0x1f;
        work->shade.a += decoded->shade.dadx & ~0x1f;
    }
}

static inline sr_result scalar_depth_test(sr_memory *memory,
                                          const rdp_primitive_state *primitive,
                                          const rdp_span_work *cursor,
                                          int x,
                                          rdp_depth_result *result)
{
    const rdp_depth_state *depth = &primitive->fragment.depth;
    *result = (rdp_depth_result){ .pass = true };
    if ((!primitive->triangle.has_depth && !depth->source_primitive) || depth->image_address == 0) {
        return SR_OK;
    }
    if (primitive->metrics) primitive->metrics->depth_tests++;

    uint16_t old_depth = 0xffffu;
    int64_t linear = cursor->depth_fixed;
    uint16_t new_depth;
    if (depth->source_primitive) {
        new_depth = depth->primitive_depth & 0x7fffu;
    } else if (linear <= 0) {
        new_depth = 0u;
    } else if (linear >= 0xffff0000ll) {
        new_depth = 0xffffu;
    } else {
        new_depth = (uint16_t)((uint64_t)linear >> 16);
    }
    const uint32_t pixel = (uint32_t)cursor->y * primitive->framebuffer.color_image.width + (uint32_t)x;
    const uint32_t addr = depth->image_address + pixel * 2u;

    if ((depth->compare || depth->update) &&
        !sr_memory_read_be16(memory, addr, &old_depth)) {
        return SR_ERROR_INVALID_ARGUMENT;
    }
    if (depth->compare) result->pass = new_depth <= old_depth;
    if (result->pass && depth->update) {
        result->update = true;
        result->address = addr;
        result->compressed = new_depth;
        if (primitive->metrics) primitive->metrics->depth_updates_planned++;
    }
    if (primitive->metrics) {
        if (result->pass) primitive->metrics->depth_passes++;
        else primitive->metrics->depth_rejects++;
    }
    return SR_OK;
}

static inline sr_result commit_depth_update(sr_memory *memory,
                                            const rdp_depth_result *depth,
                                            bool fragment_accepted,
                                            rdp_metrics *metrics)
{
    if (!depth->update) return SR_OK;
    if (!fragment_accepted) {
        if (metrics) metrics->depth_updates_discarded++;
        return SR_OK;
    }
    if (!sr_memory_write_be16(memory, depth->address, depth->compressed))
        return SR_ERROR_INVALID_ARGUMENT;
    if (metrics) metrics->depth_updates_committed++;
    return SR_OK;
}

sr_result pipeline_render_fill_triangle_span(sr_memory *memory,
                                             const rdp_primitive_state *primitive,
                                             const rdp_span_work *work)
{
    for (int x = work->x_begin; x <= work->x_end; x++) {
        sr_result result = framebuffer_write_fill_pixel(memory,
                                                        &primitive->framebuffer,
                                                        (uint32_t)x,
                                                        (uint32_t)work->y);
        if (result != SR_OK) {
            return result;
        }
    }
    return SR_OK;
}

sr_result pipeline_render_depth_triangle_span(sr_memory *memory,
                                               const rdp_primitive_state *primitive,
                                               const rdp_span_work *work)
{
    rdp_span_work cursor = *work;
    for (int x = cursor.x_begin; x <= cursor.x_end; x++) {
        rdp_depth_result depth;
        sr_result result = scalar_depth_test(memory, primitive, &cursor, x, &depth);
        if (result != SR_OK) return result;
        if (depth.pass) {
            const pipeline_inputs inputs = {
                .primitive = primitive->color.primitive_color,
                .environment = primitive->color.environment_color,
                .primitive_lod_fraction = primitive->color.primitive_lod_fraction
            };
            bool accepted;
            const rdp_color color = pipeline_shade_pixel(&primitive->color, &inputs).color;
            result = pipeline_write_fragment(memory, primitive, (uint32_t)x,
                                             (uint32_t)cursor.y, color, 0u, &accepted);
            if (result != SR_OK) return result;
            result = commit_depth_update(memory, &depth, accepted, primitive->metrics);
            if (result != SR_OK) return result;
        }
        triangle_span_work_step(&primitive->triangle, primitive, &cursor);
    }
    return SR_OK;
}

sr_result pipeline_render_shade_triangle_span(sr_memory *memory,
                                              const rdp_primitive_state *primitive,
                                              const rdp_span_work *work)
{
    rdp_span_work cursor = *work;
    for (int x = cursor.x_begin; x <= cursor.x_end; x++) {
        const rdp_color color = combine_shade_pixel(primitive, &cursor.shade);
        sr_result result = pipeline_write_fragment(memory, primitive, (uint32_t)x,
                                                   (uint32_t)cursor.y, color,
                                                   shade_base_color(&cursor.shade).a, NULL);
        if (result != SR_OK) {
            return result;
        }
        triangle_span_work_step(&primitive->triangle, primitive, &cursor);
    }
    return SR_OK;
}

sr_result pipeline_render_shade_depth_triangle_span(sr_memory *memory,
                                                    const rdp_primitive_state *primitive,
                                                    const rdp_span_work *work)
{
    rdp_span_work cursor = *work;
    for (int x = cursor.x_begin; x <= cursor.x_end; x++) {
        rdp_depth_result depth;
        sr_result result = scalar_depth_test(memory, primitive, &cursor, x, &depth);
        if (result != SR_OK) {
            return result;
        }
        if (depth.pass) {
            const rdp_color color = combine_shade_pixel(primitive, &cursor.shade);
            bool accepted;
            result = pipeline_write_fragment(memory, primitive, (uint32_t)x,
                                             (uint32_t)cursor.y, color,
                                             shade_base_color(&cursor.shade).a, &accepted);
            if (result != SR_OK) {
                return result;
            }
            result = commit_depth_update(memory, &depth, accepted, primitive->metrics);
            if (result != SR_OK) return result;
        }
        triangle_span_work_step(&primitive->triangle, primitive, &cursor);
    }
    return SR_OK;
}

/*
 * Span processing is the intended long-term raster pipeline shape, not a
 * one-off optimization. Rasterization should produce spans; pipeline stages
 * should consume spans with incremental shade/texture/depth state. As coverage,
 * blending, LOD, fog, and cycle modes grow, split this into explicit span setup,
 * texture, combine, depth/blend, and framebuffer stages rather than returning
 * to per-pixel full interpolation.
 */
sr_result pipeline_render_triangle_span(sr_memory *memory,
                                        const rdp_primitive_state *primitive,
                                        const rdp_span_work *work)
{
    if (!memory || !primitive || !work) {
        return SR_ERROR_INVALID_ARGUMENT;
    }
    if (work->x_end < work->x_begin) {
        return SR_OK;
    }

    const raster_decoded_triangle *decoded = &primitive->triangle;
    const rdp_texture_sample_state *texture = &primitive->texture;
    const rdp_color_pipeline_state *color_pipeline = &primitive->color;
    const tmem_state *tmem = primitive->tmem;
    rdp_metrics *metrics = primitive->metrics;
    rdp_span_work cursor = *work;

    for (int x = cursor.x_begin; x <= cursor.x_end; x++) {
        rdp_depth_result depth;
        sr_result depth_result = scalar_depth_test(memory, primitive, &cursor, x, &depth);
        if (depth_result != SR_OK) {
            return depth_result;
        }

        bool fragment_accepted = false;
        if (depth.pass) {
            if (decoded->has_texture) {
                const bool needs_texel0 = color_pipeline->needs_texel0;
                bool wrote_color = false;
                if (needs_texel0) {
                    int32_t s_fixed = cursor.s_fixed;
                    int32_t t_fixed = cursor.t_fixed;
                    if (texture->perspective) {
                        s_fixed = perspective_divide_coord(s_fixed, cursor.w_fixed);
                        t_fixed = perspective_divide_coord(t_fixed, cursor.w_fixed);
                    } else {
                        s_fixed = triangle_coord_fixed5(s_fixed);
                        t_fixed = triangle_coord_fixed5(t_fixed);
                    }

                    rdp_color texel0;
                    record_texture_sample_fixed(metrics, s_fixed, t_fixed, texture->perspective ? cursor.w_fixed : 0);
                    record_texture_sample_coord(metrics, texture_coord_to_texel(s_fixed), texture_coord_to_texel(t_fixed));
                    record_texture_sample_attempt(metrics, texture, color_pipeline);
                    if (tmem_sample_color_fixed5(tmem, texture, s_fixed, t_fixed, &texel0)) {
                        record_texture_sample_hit(metrics, &texture->tile);
                        record_texture_sample_color(metrics, texel0);
                        const pipeline_inputs inputs = {
                            .shade = decoded->has_shade ? shade_base_color(&cursor.shade) : (rdp_color){0, 0, 0, 0},
                            .texel0 = texel0,
                            .texel1 = texel0,
                            .primitive = color_pipeline->primitive_color,
                            .environment = color_pipeline->environment_color,
                            .primitive_lod_fraction = color_pipeline->primitive_lod_fraction
                        };
                        const rdp_color color = pipeline_shade_pixel(color_pipeline, &inputs).color;
                        sr_result result = pipeline_write_fragment(memory, primitive, (uint32_t)x,
                                                                  (uint32_t)cursor.y, color,
                                                                  inputs.shade.a, &fragment_accepted);
                        if (result != SR_OK) {
                            return result;
                        }
                        wrote_color = true;
                    } else {
                        if (metrics) {
                            metrics->texture_sample_misses++;
                        }
                    }
                } else {
                    const pipeline_inputs inputs = {
                        .shade = decoded->has_shade ? shade_base_color(&cursor.shade) : (rdp_color){0, 0, 0, 0},
                        .primitive = color_pipeline->primitive_color,
                        .environment = color_pipeline->environment_color,
                        .primitive_lod_fraction = color_pipeline->primitive_lod_fraction
                    };
                    sr_result result = pipeline_write_fragment(memory, primitive, (uint32_t)x,
                                                               (uint32_t)cursor.y,
                                                               pipeline_shade_pixel(color_pipeline, &inputs).color,
                                                               inputs.shade.a, &fragment_accepted);
                    if (result != SR_OK) {
                        return result;
                    }
                    wrote_color = true;
                }

                if (!wrote_color && decoded->has_shade) {
                    if (metrics) {
                        metrics->texture_sample_shade_fallbacks++;
                    }
                    const rdp_color shade = shade_base_color(&cursor.shade);
                    sr_result result = pipeline_write_fragment(memory, primitive, (uint32_t)x,
                                                               (uint32_t)cursor.y, shade, shade.a,
                                                               &fragment_accepted);
                    if (result != SR_OK) {
                        return result;
                    }
                }
            } else if (decoded->has_shade) {
                const rdp_color shade = shade_base_color(&cursor.shade);
                sr_result result = pipeline_write_fragment(memory, primitive, (uint32_t)x,
                                                           (uint32_t)cursor.y, shade, shade.a,
                                                           &fragment_accepted);
                if (result != SR_OK) {
                    return result;
                }
            }
        }

        depth_result = commit_depth_update(memory, &depth, fragment_accepted, primitive->metrics);
        if (depth_result != SR_OK) return depth_result;

        triangle_span_work_step(decoded, primitive, &cursor);
    }

    return SR_OK;
}

rdp_span_kernel_kind pipeline_select_triangle_kernel(const rdp_primitive_state *primitive)
{
    if (!primitive) {
        return RDP_SPAN_KERNEL_INVALID;
    }
    if (primitive->fill_mode) {
        return RDP_SPAN_KERNEL_FILL_TRIANGLE;
    }
    if (primitive->triangle.has_depth && !primitive->triangle.has_shade &&
        !primitive->triangle.has_texture) {
        return RDP_SPAN_KERNEL_DEPTH_TRIANGLE;
    }
    if (primitive->triangle.has_shade && !primitive->triangle.has_texture) {
        return (primitive->triangle.has_depth || primitive->fragment.depth.source_primitive)
            ? RDP_SPAN_KERNEL_SHADE_DEPTH_TRIANGLE
                                             : RDP_SPAN_KERNEL_SHADE_TRIANGLE;
    }
    return RDP_SPAN_KERNEL_TEXTURE_TRIANGLE;
}

static sr_result render_rectangle_pixel(sr_memory *memory,
                                        const rdp_primitive_state *primitive,
                                        int32_t s_fixed,
                                        int32_t t_fixed,
                                        uint32_t x,
                                        uint32_t y)
{
    const tmem_state *tmem = primitive->tmem;
    const rdp_texture_sample_state *texture = &primitive->texture;
    const rdp_color_pipeline_state *color_pipeline = &primitive->color;
    rdp_metrics *metrics = primitive->metrics;
    if (!color_pipeline->needs_texel0 && !color_pipeline->needs_texel1) {
        const pipeline_inputs inputs = {
            .primitive = color_pipeline->primitive_color,
            .environment = color_pipeline->environment_color,
            .primitive_lod_fraction = color_pipeline->primitive_lod_fraction
        };
        return pipeline_write_fragment(memory, primitive, x, y,
                                       pipeline_shade_pixel(color_pipeline, &inputs).color, 0u, NULL);
    }

    rdp_color texel0;
    if (metrics) {
        metrics->rect_texture_sample_attempts++;
    }
    if (!tmem_sample_color_fixed5(tmem, texture, s_fixed, t_fixed, &texel0)) {
        if (metrics) {
            metrics->rect_texture_sample_misses++;
        }
        return SR_ERROR_INVALID_ARGUMENT;
    }
    if (metrics) {
        metrics->rect_texture_sample_hits++;
    }

    pipeline_inputs inputs = {
        .texel0 = texel0,
        .texel1 = texel0,
        .primitive = color_pipeline->primitive_color,
        .environment = color_pipeline->environment_color,
        .primitive_lod_fraction = color_pipeline->primitive_lod_fraction
    };
    pipeline_outputs outputs = pipeline_shade_pixel(color_pipeline, &inputs);
    return pipeline_write_fragment(memory, primitive, x, y, outputs.color, 0u, NULL);
}

sr_result pipeline_render_rectangle_span(sr_memory *memory,
                                         const rdp_primitive_state *primitive,
                                         const rdp_span_work *work)
{
    if (!memory || !primitive || !work) {
        return SR_ERROR_INVALID_ARGUMENT;
    }

    int32_t s_fixed = work->s_fixed;
    int32_t t_fixed = work->t_fixed;
    for (int x = work->x_begin; x <= work->x_end; x++) {
        sr_result result = render_rectangle_pixel(memory,
                                                  primitive,
                                                  s_fixed,
                                                  t_fixed,
                                                  (uint32_t)x,
                                                  (uint32_t)work->y);
        if (result != SR_OK) {
            return result;
        }
        s_fixed += work->dsdx_fixed;
        t_fixed += work->dtdx_fixed;
    }

    return SR_OK;
}

sr_result pipeline_render_copy_rectangle_span(sr_memory *memory,
                                               const rdp_primitive_state *primitive,
                                               const rdp_span_work *work)
{
    if (!memory || !primitive || !work) return SR_ERROR_INVALID_ARGUMENT;
    int32_t s_fixed = work->s_fixed;
    int32_t t_fixed = work->t_fixed;
    for (int x = work->x_begin; x <= work->x_end; x++) {
        rdp_color texel;
        if (!tmem_sample_color_fixed5(primitive->tmem, &primitive->texture,
                                      s_fixed, t_fixed, &texel))
            return SR_ERROR_INVALID_ARGUMENT;

        bool passes_alpha = true;
        if (primitive->fragment.blend.alpha_compare) {
            if (primitive->framebuffer.color_image.size == RDP_SIZE_16BPP)
                passes_alpha = texel.a != 0u;
            else
                passes_alpha = texel.a >= primitive->fragment.blend.blend_color.a;
        }
        if (passes_alpha) {
            const sr_result result = framebuffer_write_color(memory, &primitive->framebuffer,
                                                             (uint32_t)x, (uint32_t)work->y,
                                                             texel);
            if (result != SR_OK) return result;
        }
        s_fixed += work->dsdx_fixed;
        t_fixed += work->dtdx_fixed;
    }
    return SR_OK;
}
