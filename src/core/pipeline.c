#include "pipeline.h"
#include <string.h>
#include <immintrin.h>
#include "framebuffer.h"
#include "rdp_memory.h"
#include "tmem.h"
#include "fragment.h"

pipeline_outputs pipeline_combine_pixel(const rdp_color_pipeline_state *state, const pipeline_inputs *inputs)
{
    pipeline_outputs out = { .coverage = 7 };
    if (!state || !inputs) {
        out.color = (rdp_color){0, 0, 0, 255};
        return out;
    }
    pipeline_inputs local_inputs = *inputs;
    local_inputs.k4 = (uint16_t)state->convert_k4;
    local_inputs.k5 = (uint16_t)state->convert_k5;
    out.color = rdp_combiner_evaluate(&state->program, state->cycle_type, &local_inputs);
    return out;
}

static uint8_t shade_component_to_u8(int32_t value)
{
    int32_t component = value >> 16;
    return component < 0 ? 0 : (component > 255 ? 255 : (uint8_t)component);
}

static int32_t triangle_coord_fixed5(int32_t interpolated)
{
    return (int16_t)((uint32_t)interpolated >> 16);
}

static void perspective_divide_packet(const int32_t *restrict coord,
                                      const int32_t *restrict w,
                                      int32_t *restrict output,
                                      uint32_t count)
{
    uint32_t chunks = (count + 3u) >> 2;
    __m128i zero = _mm_setzero_si128();
    
    for (uint32_t chunk = 0; chunk < chunks; chunk++) {
        uint32_t offset = chunk << 2;
        
        __m128i coord_i = _mm_loadu_si128((const __m128i *)&coord[offset]);
        __m128i w_i = _mm_loadu_si128((const __m128i *)&w[offset]);
        
        __m256d coord_d = _mm256_cvtepi32_pd(coord_i);
        __m256d w_d = _mm256_cvtepi32_pd(w_i);
        
        __m256d coord_scaled = _mm256_mul_pd(coord_d, _mm256_set1_pd(32768.0));
        __m256d div = _mm256_div_pd(coord_scaled, w_d);
        
        __m256d clamped = _mm256_max_pd(div, _mm256_set1_pd(-65536.0));
        clamped = _mm256_min_pd(clamped, _mm256_set1_pd(65535.0));
        
        __m128i res = _mm256_cvttpd_epi32(clamped);
        
        __m128i is_zero = _mm_cmpeq_epi32(w_i, zero);
        __m128i is_neg = _mm_cmpgt_epi32(zero, w_i);
        __m128i w_le_0 = _mm_or_si128(is_zero, is_neg);
        
        __m128i final_res = _mm_blendv_epi8(res, _mm_set1_epi32(0x7fff), w_le_0);
        
        _mm_storeu_si128((__m128i *)&output[offset], final_res);
    }
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

typedef struct rdp_depth_result {
    bool pass;
    bool update;
    uint32_t address;
    uint16_t compressed;
} rdp_depth_result;

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
    if (depth->compare) {
        result->pass = new_depth <= old_depth;
        if (primitive->metrics) {
            primitive->metrics->fragment_depth_tests++;
            if (!result->pass) primitive->metrics->fragment_depth_rejects++;
        }
    }
    if (result->pass && depth->update) {
        result->update = true;
        result->address = addr;
        result->compressed = new_depth;
    }
    return SR_OK;
}

static inline sr_result commit_depth_update(sr_memory *memory,
                                            const rdp_depth_result *depth,
                                            bool fragment_accepted)
{
    if (!depth->update) return SR_OK;
    if (!fragment_accepted) {
        return SR_OK;
    }
    if (!sr_memory_write_be16(memory, depth->address, depth->compressed))
        return SR_ERROR_INVALID_ARGUMENT;
    return SR_OK;
}

typedef struct pipeline_triangle_block {
    uint32_t count;
    int x[RDP_PACKET_LANES];
    int64_t depth_fixed[RDP_PACKET_LANES];
    int32_t s[RDP_PACKET_LANES], t[RDP_PACKET_LANES], w[RDP_PACKET_LANES];
    int32_t sample_s[RDP_PACKET_LANES], sample_t[RDP_PACKET_LANES];
    int32_t shade[4][RDP_PACKET_LANES];
    rdp_depth_result depth[RDP_PACKET_LANES];
    rdp_combiner_packet combiner;
    uint16_t live_mask;
    uint16_t fallback_mask;
} pipeline_triangle_block;

static void setup_triangle_block(const rdp_primitive_state *primitive,
                                 rdp_span_work *cursor,
                                 uint32_t count,
                                 pipeline_triangle_block *block)
{
    block->count = count;
    block->combiner.count = count;
    block->live_mask = count == RDP_PACKET_LANES ? 0xffffu : (uint16_t)((1u << count) - 1u);
    block->fallback_mask = 0u;
    const raster_decoded_triangle *decoded = &primitive->triangle;
    const int32_t dr[4] = { decoded->shade.drdx & ~0x1f, decoded->shade.dgdx & ~0x1f,
                            decoded->shade.dbdx & ~0x1f, decoded->shade.dadx & ~0x1f };
    const int32_t base[4] = { cursor->shade.r, cursor->shade.g, cursor->shade.b, cursor->shade.a };
    for (uint32_t lane = 0; lane < count; lane++) {
        block->x[lane] = cursor->x_begin + (int)lane;
        block->depth_fixed[lane] = cursor->depth_fixed + (int64_t)lane * decoded->depth.dzdx;
        block->s[lane] = (int32_t)((uint32_t)cursor->s_fixed + lane * (uint32_t)decoded->texture.dsdx);
        block->t[lane] = (int32_t)((uint32_t)cursor->t_fixed + lane * (uint32_t)decoded->texture.dtdx);
        block->w[lane] = (int32_t)((uint32_t)cursor->w_fixed + lane * (uint32_t)decoded->texture.dwdx);
        for (uint32_t component = 0; component < 4u; component++) {
            block->shade[component][lane] = (int32_t)((uint32_t)base[component] + lane * (uint32_t)dr[component]);
            block->combiner.shade[component][lane] = shade_component_to_u8(block->shade[component][lane]);
        }
    }
    if (decoded->has_depth) cursor->depth_fixed += (int64_t)count * decoded->depth.dzdx;
    if (decoded->has_texture && primitive->color.needs_texel0) {
        cursor->s_fixed = (int32_t)((uint32_t)cursor->s_fixed + count * (uint32_t)decoded->texture.dsdx);
        cursor->t_fixed = (int32_t)((uint32_t)cursor->t_fixed + count * (uint32_t)decoded->texture.dtdx);
        if (primitive->texture.perspective)
            cursor->w_fixed = (int32_t)((uint32_t)cursor->w_fixed + count * (uint32_t)decoded->texture.dwdx);
    }
    if (decoded->has_shade) {
        cursor->shade.r = (int32_t)((uint32_t)cursor->shade.r + count * (uint32_t)dr[0]);
        cursor->shade.g = (int32_t)((uint32_t)cursor->shade.g + count * (uint32_t)dr[1]);
        cursor->shade.b = (int32_t)((uint32_t)cursor->shade.b + count * (uint32_t)dr[2]);
        cursor->shade.a = (int32_t)((uint32_t)cursor->shade.a + count * (uint32_t)dr[3]);
    }
    cursor->x_begin += (int)count;
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
    const int total = work->x_end - work->x_begin + 1;

    for (int offset = 0; offset < total; offset += (int)RDP_PACKET_LANES) {
        pipeline_triangle_block block = {0};
        const uint32_t count = (uint32_t)(total - offset) < RDP_PACKET_LANES
            ? (uint32_t)(total - offset) : RDP_PACKET_LANES;
        setup_triangle_block(primitive, &cursor, count, &block);

        if (primitive->fill_mode) {
            for (uint32_t lane = 0; lane < count; lane++) {
                const sr_result result = framebuffer_write_fill_pixel(memory,
                    &primitive->framebuffer, (uint32_t)block.x[lane], (uint32_t)work->y);
                if (result != SR_OK) return result;
            }
            continue;
        }

        for (uint32_t lane = 0; lane < count; lane++) {
            rdp_span_work lane_work = {
                .y = work->y,
                .depth_fixed = block.depth_fixed[lane]
            };
            const sr_result result = scalar_depth_test(memory, primitive,
                &lane_work, block.x[lane], &block.depth[lane]);
            if (result != SR_OK) return result;
            if (!block.depth[lane].pass) block.live_mask &= (uint16_t)~(1u << lane);
        }

        if (decoded->has_texture && color_pipeline->needs_texel0) {
            if (texture->perspective) {
                perspective_divide_packet(block.s, block.w, block.sample_s, count);
                perspective_divide_packet(block.t, block.w, block.sample_t, count);
            } else {
                for (uint32_t lane = 0; lane < count; lane++) {
                    block.sample_s[lane] = triangle_coord_fixed5(block.s[lane]);
                    block.sample_t[lane] = triangle_coord_fixed5(block.t[lane]);
                }
            }
            for (uint32_t lane = 0; lane < count; lane++) {
                const uint16_t bit = (uint16_t)(1u << lane);
                if (!(block.live_mask & bit)) continue;
                const int32_t s = block.sample_s[lane];
                const int32_t t = block.sample_t[lane];
                record_texture_sample_fixed(metrics, s, t,
                    texture->perspective ? block.w[lane] : 0);
                record_texture_sample_coord(metrics, texture_coord_to_texel(s), texture_coord_to_texel(t));
                record_texture_sample_attempt(metrics, texture, color_pipeline);
                rdp_color texel;
                if (tmem_sample_color_fixed5(tmem, texture, s, t, &texel)) {
                    record_texture_sample_hit(metrics, &texture->tile);
                    record_texture_sample_color(metrics, texel);
                    block.combiner.texel0[0][lane] = texel.r;
                    block.combiner.texel0[1][lane] = texel.g;
                    block.combiner.texel0[2][lane] = texel.b;
                    block.combiner.texel0[3][lane] = texel.a;
                    for (uint32_t component = 0; component < 4u; component++)
                        block.combiner.texel1[component][lane] = block.combiner.texel0[component][lane];
                } else if (decoded->has_shade) {
                    if (metrics) {
                        metrics->texture_sample_misses++;
                        metrics->texture_sample_shade_fallbacks++;
                    }
                    block.fallback_mask |= bit;
                } else {
                    if (metrics) metrics->texture_sample_misses++;
                    block.live_mask &= (uint16_t)~bit;
                }
            }
        }

        rdp_combiner_evaluate_packet(color_pipeline, &block.combiner);
        rdp_fragment_packet fragments = {
            .count = count,
            .active_mask = block.live_mask
        };
        for (uint32_t lane = 0; lane < count; lane++) {
            const uint16_t bit = (uint16_t)(1u << lane);
            fragments.x[lane] = (uint32_t)block.x[lane];
            fragments.y[lane] = (uint32_t)work->y;
            fragments.shade_alpha[lane] = block.combiner.shade[3][lane];
            for (uint32_t component = 0; component < 4u; component++)
                fragments.color[component][lane] = block.fallback_mask & bit
                    ? block.combiner.shade[component][lane]
                    : block.combiner.output[component][lane];
        }
        sr_result result = fragment_finish_packet(memory, primitive, &fragments);
        if (result != SR_OK) return result;
        for (uint32_t lane = 0; lane < count; lane++) {
            result = commit_depth_update(memory, &block.depth[lane],
                (fragments.accepted_mask & (uint16_t)(1u << lane)) != 0u);
            if (result != SR_OK) return result;
        }
    }

    return SR_OK;
}

sr_result pipeline_render_rectangle_span(sr_memory *memory,
                                         const rdp_primitive_state *primitive,
                                         const rdp_span_work *work)
{
    if (!memory || !primitive || !work) {
        return SR_ERROR_INVALID_ARGUMENT;
    }

    const rdp_color_pipeline_state *color = &primitive->color;
    const bool needs_texture = color->needs_texel0 || color->needs_texel1;
    const uint32_t total = (uint32_t)(work->x_end - work->x_begin + 1);
    for (uint32_t offset = 0; offset < total; offset += RDP_PACKET_LANES) {
        const uint32_t count = total - offset < RDP_PACKET_LANES
            ? total - offset : RDP_PACKET_LANES;
        rdp_combiner_packet packet = { .count = count };
        uint16_t live_mask = count == RDP_PACKET_LANES ? 0xffffu
            : (uint16_t)((1u << count) - 1u);
        if (needs_texture) {
            for (uint32_t lane = 0; lane < count; lane++) {
                const int32_t s = (int32_t)((uint32_t)work->s_fixed +
                    (offset + lane) * (uint32_t)work->dsdx_fixed);
                const int32_t t = (int32_t)((uint32_t)work->t_fixed +
                    (offset + lane) * (uint32_t)work->dtdx_fixed);
                rdp_color texel;
                if (primitive->metrics) primitive->metrics->rect_texture_sample_attempts++;
                if (!tmem_sample_color_fixed5(primitive->tmem, &primitive->texture,
                                              s, t, &texel)) {
                    live_mask &= (uint16_t)~(1u << lane);
                    if (primitive->metrics) primitive->metrics->rect_texture_sample_misses++;
                    continue;
                }
                if (primitive->metrics) primitive->metrics->rect_texture_sample_hits++;
                packet.texel0[0][lane] = packet.texel1[0][lane] = texel.r;
                packet.texel0[1][lane] = packet.texel1[1][lane] = texel.g;
                packet.texel0[2][lane] = packet.texel1[2][lane] = texel.b;
                packet.texel0[3][lane] = packet.texel1[3][lane] = texel.a;
            }
        }
        rdp_combiner_evaluate_packet(color, &packet);
        rdp_fragment_packet fragments = {
            .count = count,
            .active_mask = live_mask
        };
        for (uint32_t lane = 0; lane < count; lane++) {
            fragments.x[lane] = (uint32_t)work->x_begin + offset + lane;
            fragments.y[lane] = (uint32_t)work->y;
            for (uint32_t component = 0; component < 4u; component++)
                fragments.color[component][lane] = packet.output[component][lane];
        }
        const sr_result result = fragment_finish_packet(memory, primitive, &fragments);
        if (result != SR_OK) return result;
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
                                      s_fixed, t_fixed, &texel)) {
            s_fixed += work->dsdx_fixed;
            t_fixed += work->dtdx_fixed;
            continue;
        }

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
