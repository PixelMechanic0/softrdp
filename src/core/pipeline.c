#include "pipeline.h"
#include <string.h>
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
    /* Keep this as one alias-free SoA loop: Clang/GCC can vectorize the
     * independent binary64 divisions directly, while retaining a portable C
     * implementation and handling arbitrary packet tails without SIMD reads. */
    for (uint32_t lane = 0; lane < count; lane++) {
        if (w[lane] <= 0) {
            output[lane] = 0x7fff;
            continue;
        }
        const double divided = ((double)coord[lane] * 32768.0) / (double)w[lane];
        output[lane] = divided < -65536.0 ? -65536 :
                       divided > 65535.0 ? 65535 : (int32_t)divided;
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
                                          uint32_t addr,
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
    if ((depth->compare || depth->update) &&
        !sr_memory_read_be16(memory, addr, &old_depth)) {
        return SR_ERROR_INVALID_ARGUMENT;
    }
    if (depth->compare) {
        result->pass = new_depth <= old_depth;
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

static void setup_triangle_block(const rdp_primitive_state *primitive,
                                 rdp_span_work *cursor,
                                 uint32_t count,
                                 rdp_fragment_block *block)
{
    block->count = count;
    block->active_mask = count == RDP_PACKET_LANES ? 0xffffu : (uint16_t)((1u << count) - 1u);
    block->fallback_mask = 0u;
    block->depth_update_mask = 0u;
    block->y = (uint32_t)cursor->y;
    const uint32_t first_pixel = (uint32_t)cursor->y * primitive->framebuffer.color_image.width +
                                 (uint32_t)cursor->x_begin;
    const uint32_t first_color_address = primitive->framebuffer.color_image.address +
                                         first_pixel * primitive->framebuffer.bytes_per_pixel;
    const raster_decoded_triangle *decoded = &primitive->triangle;
    const int32_t dr[4] = { decoded->shade.drdx & ~0x1f, decoded->shade.dgdx & ~0x1f,
                            decoded->shade.dbdx & ~0x1f, decoded->shade.dadx & ~0x1f };
    const int32_t base[4] = { cursor->shade.r, cursor->shade.g, cursor->shade.b, cursor->shade.a };
    for (uint32_t lane = 0; lane < count; lane++) {
        block->x[lane] = cursor->x_begin + (int)lane;
        block->color_address[lane] = first_color_address + lane * primitive->framebuffer.bytes_per_pixel;
        block->depth_fixed[lane] = cursor->depth_fixed + (int64_t)lane * decoded->depth.dzdx;
        block->s[lane] = (int32_t)((uint32_t)cursor->s_fixed + lane * (uint32_t)decoded->texture.dsdx);
        block->t[lane] = (int32_t)((uint32_t)cursor->t_fixed + lane * (uint32_t)decoded->texture.dtdx);
        block->w[lane] = (int32_t)((uint32_t)cursor->w_fixed + lane * (uint32_t)decoded->texture.dwdx);
        for (uint32_t component = 0; component < 4u; component++) {
            const int32_t interpolated = (int32_t)((uint32_t)base[component] + lane * (uint32_t)dr[component]);
            block->shade[component][lane] = shade_component_to_u8(interpolated);
        }
        block->lod_fraction[lane] = 0u;
    }
    if (decoded->has_depth) cursor->depth_fixed += (int64_t)count * decoded->depth.dzdx;
    if (primitive->block_plan.stages & RDP_BLOCK_STAGE_TEXTURE) {
        cursor->s_fixed = (int32_t)((uint32_t)cursor->s_fixed + count * (uint32_t)decoded->texture.dsdx);
        cursor->t_fixed = (int32_t)((uint32_t)cursor->t_fixed + count * (uint32_t)decoded->texture.dtdx);
        if (primitive->block_plan.coordinates == RDP_BLOCK_COORD_PERSPECTIVE)
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
    rdp_span_work cursor = *work;
    const int total = work->x_end - work->x_begin + 1;

    for (int offset = 0; offset < total; offset += (int)RDP_PACKET_LANES) {
        rdp_fragment_block block;
        const uint32_t count = (uint32_t)(total - offset) < RDP_PACKET_LANES
            ? (uint32_t)(total - offset) : RDP_PACKET_LANES;
        setup_triangle_block(primitive, &cursor, count, &block);

        if (primitive->block_plan.stages & RDP_BLOCK_STAGE_FILL) {
            for (uint32_t lane = 0; lane < count; lane++) {
                const sr_result result = framebuffer_write_fill_pixel(memory,
                    &primitive->framebuffer, (uint32_t)block.x[lane], (uint32_t)work->y);
                if (result != SR_OK) return result;
            }
            continue;
        }

        if (primitive->block_plan.stages & RDP_BLOCK_STAGE_DEPTH) {
            for (uint32_t lane = 0; lane < count; lane++) {
                rdp_span_work lane_work = {
                    .y = work->y,
                    .depth_fixed = block.depth_fixed[lane]
                };
                rdp_depth_result depth;
                const uint32_t depth_address = primitive->fragment.depth.image_address +
                    ((uint32_t)work->y * primitive->framebuffer.color_image.width + block.x[0]) * 2u + lane * 2u;
                const sr_result result = scalar_depth_test(memory, primitive,
                    &lane_work, depth_address, &depth);
                if (result != SR_OK) return result;
                if (!depth.pass) block.active_mask &= (uint16_t)~(1u << lane);
                if (depth.update) {
                    block.depth_update_mask |= (uint16_t)(1u << lane);
                    block.depth_address[lane] = depth.address;
                    block.depth_value[lane] = depth.compressed;
                }
            }
        }

        if (primitive->block_plan.stages & RDP_BLOCK_STAGE_TEXTURE) {
            if (primitive->block_plan.coordinates == RDP_BLOCK_COORD_PERSPECTIVE) {
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
                if (!(block.active_mask & bit)) continue;
                const int32_t s = block.sample_s[lane];
                const int32_t t = block.sample_t[lane];
                rdp_color texel;
                if (tmem_sample_color_fixed5(tmem, texture, s, t, &texel)) {
                    block.texel0[0][lane] = texel.r;
                    block.texel0[1][lane] = texel.g;
                    block.texel0[2][lane] = texel.b;
                    block.texel0[3][lane] = texel.a;
                    for (uint32_t component = 0; component < 4u; component++)
                        block.texel1[component][lane] = block.texel0[component][lane];
                } else if (decoded->has_shade) {
                    block.fallback_mask |= bit;
                } else {
                    block.active_mask &= (uint16_t)~bit;
                }
            }
        }

        rdp_combiner_evaluate_packet(color_pipeline, &block);
        for (uint32_t lane = 0; lane < count; lane++) {
            const uint16_t bit = (uint16_t)(1u << lane);
            for (uint32_t component = 0; component < 4u; component++)
                if (block.fallback_mask & bit)
                    block.color[component][lane] = block.shade[component][lane];
        }
        sr_result result = fragment_finish_packet(memory, primitive, &block);
        if (result != SR_OK) return result;
        for (uint32_t lane = 0; lane < count; lane++) {
            const rdp_depth_result depth = {
                .update = (block.depth_update_mask & (uint16_t)(1u << lane)) != 0u,
                .address = block.depth_address[lane], .compressed = block.depth_value[lane]
            };
            result = commit_depth_update(memory, &depth,
                (block.accepted_mask & (uint16_t)(1u << lane)) != 0u);
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
    const bool needs_texture =
        (primitive->block_plan.stages & RDP_BLOCK_STAGE_TEXTURE) != 0u;
    const uint32_t total = (uint32_t)(work->x_end - work->x_begin + 1);
    for (uint32_t offset = 0; offset < total; offset += RDP_PACKET_LANES) {
        const uint32_t count = total - offset < RDP_PACKET_LANES
            ? total - offset : RDP_PACKET_LANES;
        rdp_fragment_block packet;
        packet.count = count;
        uint16_t live_mask = count == RDP_PACKET_LANES ? 0xffffu
            : (uint16_t)((1u << count) - 1u);
        for (uint32_t lane = 0; lane < count; lane++) {
            packet.lod_fraction[lane] = 0u;
        }
        if (needs_texture) {
            for (uint32_t lane = 0; lane < count; lane++) {
                const int32_t s = (int32_t)((uint32_t)work->s_fixed +
                    (offset + lane) * (uint32_t)work->dsdx_fixed);
                const int32_t t = (int32_t)((uint32_t)work->t_fixed +
                    (offset + lane) * (uint32_t)work->dtdx_fixed);
                rdp_color texel;
                if (!tmem_sample_color_fixed5(primitive->tmem, &primitive->texture,
                                              s, t, &texel)) {
                    live_mask &= (uint16_t)~(1u << lane);
                    continue;
                }
                packet.texel0[0][lane] = packet.texel1[0][lane] = texel.r;
                packet.texel0[1][lane] = packet.texel1[1][lane] = texel.g;
                packet.texel0[2][lane] = packet.texel1[2][lane] = texel.b;
                packet.texel0[3][lane] = packet.texel1[3][lane] = texel.a;
            }
        }
        rdp_combiner_evaluate_packet(color, &packet);
        packet.active_mask = live_mask;
        packet.y = (uint32_t)work->y;
        const uint32_t first_pixel = (uint32_t)work->y * primitive->framebuffer.color_image.width +
                                     (uint32_t)work->x_begin + offset;
        const uint32_t first_address = primitive->framebuffer.color_image.address +
                                       first_pixel * primitive->framebuffer.bytes_per_pixel;
        for (uint32_t lane = 0; lane < count; lane++) {
            packet.x[lane] = (uint32_t)work->x_begin + offset + lane;
            packet.color_address[lane] = first_address + lane * primitive->framebuffer.bytes_per_pixel;
        }
        const sr_result result = fragment_finish_packet(memory, primitive, &packet);
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
        if (primitive->block_plan.stages & RDP_BLOCK_STAGE_ALPHA_COMPARE) {
            if (primitive->block_plan.framebuffer == RDP_BLOCK_FRAMEBUFFER_16)
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
