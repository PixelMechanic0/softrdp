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

static void perspective_divide_pair_packet(const int32_t *restrict s,
                                           const int32_t *restrict t,
                                           const int32_t *restrict w,
                                           int32_t *restrict output_s,
                                           int32_t *restrict output_t,
                                           bool *restrict lod_clamp,
                                           uint32_t count)
{
    /* S and T share one reciprocal. Keeping this as an alias-free SoA loop
     * lets the compiler vectorize it while avoiding two divisions per pair. */
    for (uint32_t lane = 0; lane < count; lane++) {
        if (w[lane] <= 0) {
            output_s[lane] = output_t[lane] = 0x7fff;
            if (lod_clamp) lod_clamp[lane] = true;
            continue;
        }
        const double scale = 32768.0 / (double)w[lane];
        const double divided_s = (double)s[lane] * scale;
        const double divided_t = (double)t[lane] * scale;
        if (lod_clamp && (divided_s < -65536.0 || divided_s > 65535.0 ||
                          divided_t < -65536.0 || divided_t > 65535.0))
            lod_clamp[lane] = true;
        output_s[lane] = divided_s < -65536.0 ? -65536 :
                         divided_s > 65535.0 ? 65535 : (int32_t)divided_s;
        output_t[lane] = divided_t < -65536.0 ? -65536 :
                         divided_t > 65535.0 ? 65535 : (int32_t)divided_t;
    }
}

typedef struct rdp_lod_result {
    uint16_t fraction;
    uint8_t tile0;
    uint8_t tile1;
} rdp_lod_result;

static int32_t sign_extend_17(int32_t value)
{
    const uint32_t bits = (uint32_t)value & 0x1ffffu;
    return (int32_t)((bits ^ 0x10000u) - 0x10000u);
}

static uint32_t lod_delta(int32_t scurr, int32_t snext,
                          int32_t tcurr, int32_t tnext,
                          uint32_t previous)
{
    int32_t ds = sign_extend_17(snext) - sign_extend_17(scurr);
    int32_t dt = sign_extend_17(tnext) - sign_extend_17(tcurr);
    if (((uint32_t)ds & 0x20000u) != 0u) ds = (int32_t)(~ds & 0x1ffff);
    if (((uint32_t)dt & 0x20000u) != 0u) dt = (int32_t)(~dt & 0x1ffff);
    uint32_t delta = (uint32_t)(ds > dt ? ds : dt);
    if (previous > delta) delta = previous;
    uint32_t lod = delta & 0x7fffu;
    if (delta & 0x1c000u) lod |= 0x4000u;
    return lod;
}

static uint32_t lod_log2_level(uint32_t value)
{
    uint32_t level = 0u;
    while (value >>= 1u) level++;
    return level;
}

static rdp_lod_result resolve_lod(const rdp_primitive_state *primitive,
                                  int32_t s, int32_t t,
                                  int32_t sx, int32_t tx,
                                  int32_t sy, int32_t ty,
                                  bool lod_clamp)
{
    uint32_t lod = 0u;
    if (!lod_clamp) {
        lod = lod_delta(s, sx, t, tx, 0u);
        lod = lod_delta(s, sy, t, ty, lod);
    }

    uint32_t level = 0u;
    bool magnify;
    bool distant;
    uint16_t fraction;
    if ((lod & 0x4000u) || lod_clamp) {
        magnify = false;
        distant = true;
        fraction = 0xffu;
    } else if (lod < primitive->lod_min_level) {
        magnify = true;
        distant = primitive->lod_max_level == 0u;
        if (!primitive->sharpen_lod && !primitive->detail_lod)
            fraction = distant ? 0xffu : 0u;
        else {
            fraction = (uint16_t)(primitive->lod_min_level << 3);
            if (primitive->sharpen_lod) fraction |= 0x100u;
        }
    } else if (lod < 32u) {
        magnify = true;
        distant = primitive->lod_max_level == 0u;
        if (!primitive->sharpen_lod && !primitive->detail_lod)
            fraction = distant ? 0xffu : 0u;
        else {
            fraction = (uint16_t)(lod << 3);
            if (primitive->sharpen_lod) fraction |= 0x100u;
        }
    } else {
        magnify = false;
        level = lod_log2_level((lod >> 5) & 0xffu);
        distant = primitive->lod_max_level
            ? ((lod & 0x6000u) != 0u || level >= primitive->lod_max_level)
            : true;
        fraction = (!primitive->sharpen_lod && !primitive->detail_lod && distant)
            ? 0xffu : (uint16_t)(((lod << 3) >> level) & 0xffu);
    }

    uint8_t tile0 = primitive->lod_base_tile;
    uint8_t tile1 = tile0;
    if (primitive->texture_lod) {
        if (distant) level = primitive->lod_max_level;
        if (!primitive->detail_lod) {
            tile0 = (uint8_t)((primitive->lod_base_tile + level) & 7u);
            tile1 = (distant || (!primitive->sharpen_lod && magnify))
                ? tile0 : (uint8_t)((tile0 + 1u) & 7u);
        } else {
            tile0 = (uint8_t)((primitive->lod_base_tile + level +
                               (magnify ? 0u : 1u)) & 7u);
            tile1 = (uint8_t)((primitive->lod_base_tile + level +
                               (!distant && !magnify ? 2u : 1u)) & 7u);
        }
    }
    return (rdp_lod_result){ fraction, tile0, tile1 };
}

static void store_texel(uint16_t destination[4][RDP_PACKET_LANES],
                        uint32_t lane, rdp_color texel)
{
    destination[0][lane] = texel.r;
    destination[1][lane] = texel.g;
    destination[2][lane] = texel.b;
    destination[3][lane] = texel.a;
}

typedef struct rdp_depth_result {
    bool pass;
    bool update;
    uint32_t address;
    uint16_t compressed;
} rdp_depth_result;

static inline uint32_t rdp_depth_decompress(uint16_t stored)
{
    static const uint8_t shift[8] = { 6u, 5u, 4u, 3u, 2u, 1u, 0u, 0u };
    static const uint32_t add[8] = {
        0x00000u, 0x20000u, 0x30000u, 0x38000u,
        0x3c000u, 0x3e000u, 0x3f000u, 0x3f800u
    };
    const uint32_t encoded = (stored >> 2) & 0x3fffu;
    const uint32_t exponent = encoded >> 11;
    return (((encoded & 0x7ffu) << shift[exponent]) + add[exponent]) & 0x3ffffu;
}

static inline uint16_t rdp_depth_compress(uint32_t depth)
{
    depth &= 0x3ffffu;
    if (depth < 0x20000u) return (uint16_t)((depth >> 4) & 0x1ffcu);
    if (depth < 0x30000u) return (uint16_t)(((depth >> 3) & 0x1ffcu) | 0x2000u);
    if (depth < 0x38000u) return (uint16_t)(((depth >> 2) & 0x1ffcu) | 0x4000u);
    if (depth < 0x3c000u) return (uint16_t)(((depth >> 1) & 0x1ffcu) | 0x6000u);
    if (depth < 0x3e000u) return (uint16_t)((depth & 0x1ffcu) | 0x8000u);
    if (depth < 0x3f000u) return (uint16_t)(((depth << 1) & 0x1ffcu) | 0xa000u);
    if (depth < 0x3f800u) return (uint16_t)(((depth << 2) & 0x1ffcu) | 0xc000u);
    return (uint16_t)(((depth << 2) & 0x1ffcu) | 0xe000u);
}

static inline uint32_t highest_depth_delta(uint32_t value)
{
    if (value == 0u) return 0u;
    uint32_t result = 1u;
    while (value >>= 1u) result <<= 1u;
    return result;
}

static inline uint32_t depth_delta_exponent(uint32_t value)
{
    uint32_t exponent = 0u;
    while (value >>= 1u) exponent++;
    return exponent;
}

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

    uint16_t old_stored = 0xffffu;
    int64_t linear = cursor->depth_fixed;
    uint32_t new_depth;
    if (depth->source_primitive) {
        new_depth = ((uint32_t)depth->primitive_depth << 3) & 0x3ffffu;
    } else {
        const uint32_t scaled = ((uint32_t)linear >> 13) & 0x7ffffu;
        const uint32_t overflow = (scaled >> 17) & 3u;
        new_depth = overflow < 2u ? scaled & 0x3ffffu :
                    overflow == 2u ? 0x3ffffu : 0u;
    }
    if ((depth->compare || depth->update) &&
        !sr_memory_read_be16(memory, addr, &old_stored)) {
        return SR_ERROR_INVALID_ARGUMENT;
    }
    if (depth->compare) {
        const uint32_t old_depth = rdp_depth_decompress(old_stored);
        const bool maximum = old_depth == 0x3ffffu;
        const bool in_front = new_depth < old_depth;
        const uint32_t raw_memory_delta = ((old_stored & 3u) << 2) |
                                          ((old_stored & 1u) ? 3u : 0u);
        const uint32_t delta = highest_depth_delta(depth->pixel_delta |
                                                   (1u << raw_memory_delta));
        const bool nearer = new_depth <= old_depth + delta * 8u;
        const bool farther = new_depth + delta * 8u >= old_depth;
        switch (depth->mode & 3u) {
        case 0u: result->pass = maximum || in_front; break;
        case 1u: result->pass = maximum || nearer; break;
        case 2u: result->pass = maximum || in_front; break;
        default: result->pass = !maximum && nearer && farther; break;
        }
    }
    if (result->pass && depth->update) {
        result->update = true;
        result->address = addr;
        const uint32_t delta_exponent = depth_delta_exponent(depth->pixel_delta);
        result->compressed = (uint16_t)(rdp_depth_compress(new_depth) |
                                        ((delta_exponent >> 2) & 3u));
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
    if (!(primitive->block_plan.stages & RDP_BLOCK_STAGE_TEXTURE)) {
        if (primitive->color.needs_texel0)
            memset(block->texel0, 0, sizeof(block->texel0));
        if (primitive->color.needs_texel1)
            memset(block->texel1, 0, sizeof(block->texel1));
    }
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
            const bool uses_lod = (primitive->block_plan.stages & RDP_BLOCK_STAGE_LOD) != 0u;
            bool lod_clamp[RDP_PACKET_LANES] = { false };
            if (primitive->block_plan.coordinates == RDP_BLOCK_COORD_PERSPECTIVE) {
                bool *current_clamp = uses_lod && color_pipeline->cycle_type != RDP_CYCLE_1
                    ? lod_clamp : NULL;
                perspective_divide_pair_packet(block.s, block.t, block.w,
                    block.sample_s, block.sample_t, current_clamp, count);
            } else {
                for (uint32_t lane = 0; lane < count; lane++) {
                    block.sample_s[lane] = triangle_coord_fixed5(block.s[lane]);
                    block.sample_t[lane] = triangle_coord_fixed5(block.t[lane]);
                }
            }
            int32_t next_s[RDP_PACKET_LANES];
            int32_t next_t[RDP_PACKET_LANES];
            int32_t next_y_s[RDP_PACKET_LANES];
            int32_t next_y_t[RDP_PACKET_LANES];
            if (uses_lod) {
                int32_t raw_s[RDP_PACKET_LANES], raw_t[RDP_PACKET_LANES];
                int32_t raw_y_s[RDP_PACKET_LANES], raw_y_t[RDP_PACKET_LANES];
                int32_t raw_w[RDP_PACKET_LANES], raw_y_w[RDP_PACKET_LANES];
                for (uint32_t lane = 0; lane < count; lane++) {
                    raw_s[lane] = (int32_t)((uint32_t)block.s[lane] + (uint32_t)decoded->texture.dsdx);
                    raw_t[lane] = (int32_t)((uint32_t)block.t[lane] + (uint32_t)decoded->texture.dtdx);
                    raw_w[lane] = (int32_t)((uint32_t)block.w[lane] + (uint32_t)decoded->texture.dwdx);
                    if (color_pipeline->cycle_type == RDP_CYCLE_1) {
                        raw_y_s[lane] = (int32_t)((uint32_t)block.s[lane] +
                            2u * (uint32_t)decoded->texture.dsdx);
                        raw_y_t[lane] = (int32_t)((uint32_t)block.t[lane] +
                            2u * (uint32_t)decoded->texture.dtdx);
                        raw_y_w[lane] = (int32_t)((uint32_t)block.w[lane] +
                            2u * (uint32_t)decoded->texture.dwdx);
                    } else {
                        raw_y_s[lane] = (int32_t)((uint32_t)block.s[lane] + (uint32_t)decoded->texture.dsdy);
                        raw_y_t[lane] = (int32_t)((uint32_t)block.t[lane] + (uint32_t)decoded->texture.dtdy);
                        raw_y_w[lane] = (int32_t)((uint32_t)block.w[lane] + (uint32_t)decoded->texture.dwdy);
                    }
                }
                if (primitive->block_plan.coordinates == RDP_BLOCK_COORD_PERSPECTIVE) {
                    perspective_divide_pair_packet(raw_s, raw_t, raw_w,
                        next_s, next_t, lod_clamp, count);
                    perspective_divide_pair_packet(raw_y_s, raw_y_t, raw_y_w,
                        next_y_s, next_y_t, lod_clamp, count);
                } else {
                    for (uint32_t lane = 0; lane < count; lane++) {
                        next_s[lane] = triangle_coord_fixed5(raw_s[lane]);
                        next_t[lane] = triangle_coord_fixed5(raw_t[lane]);
                        next_y_s[lane] = triangle_coord_fixed5(raw_y_s[lane]);
                        next_y_t[lane] = triangle_coord_fixed5(raw_y_t[lane]);
                    }
                }
            }
            for (uint32_t lane = 0; lane < count; lane++) {
                const uint16_t bit = (uint16_t)(1u << lane);
                if (!(block.active_mask & bit)) continue;
                const int32_t s = block.sample_s[lane], t = block.sample_t[lane];
                const rdp_texture_sample_state *texture0 = texture;
                const rdp_texture_sample_state *texture1 = &primitive->texture_cycle1;
                if (uses_lod) {
                    const rdp_lod_result lod = color_pipeline->cycle_type == RDP_CYCLE_1
                        ? resolve_lod(primitive, next_s[lane], next_t[lane],
                            next_y_s[lane], next_y_t[lane], next_s[lane], next_t[lane],
                            lod_clamp[lane])
                        : resolve_lod(primitive, s, t, next_s[lane], next_t[lane],
                            next_y_s[lane], next_y_t[lane], lod_clamp[lane]);
                    block.lod_fraction[lane] = lod.fraction;
                    texture0 = &primitive->lod_textures[lod.tile0];
                    texture1 = &primitive->lod_textures_cycle1[lod.tile1];
                }
                rdp_color texel0, texel1;
                const bool ok0 = !color_pipeline->needs_texel0 ||
                    tmem_sample_color_fixed5(tmem, texture0, s, t, &texel0);
                const bool ok1 = !color_pipeline->needs_texel1 ||
                    tmem_sample_color_fixed5(tmem, texture1, s, t, &texel1);
                if (ok0 && ok1) {
                    if (color_pipeline->needs_texel0) store_texel(block.texel0, lane, texel0);
                    if (color_pipeline->needs_texel1) store_texel(block.texel1, lane, texel1);
                    if (!color_pipeline->needs_texel1 && color_pipeline->needs_texel0)
                        store_texel(block.texel1, lane, texel0);
                    if (!color_pipeline->needs_texel0 && color_pipeline->needs_texel1)
                        store_texel(block.texel0, lane, texel1);
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
            /* Texture rectangles carry no shade coefficients. The RDP
             * edgewalker supplies zero RGBA shade attributes for every lane.
             */
            for (uint32_t component = 0; component < 4u; component++)
                packet.shade[component][lane] = 0u;
        }
        if (needs_texture) {
            for (uint32_t lane = 0; lane < count; lane++) {
                const int32_t s = (int32_t)((uint32_t)work->s_fixed +
                    (offset + lane) * (uint32_t)work->dsdx_fixed);
                const int32_t t = (int32_t)((uint32_t)work->t_fixed +
                    (offset + lane) * (uint32_t)work->dtdx_fixed);
                rdp_color texel0, texel1;
                const bool ok0 = !color->needs_texel0 ||
                    tmem_sample_color_fixed5(primitive->tmem, &primitive->texture,
                                             s, t, &texel0);
                const bool ok1 = !color->needs_texel1 ||
                    tmem_sample_color_fixed5(primitive->tmem, &primitive->texture_cycle1,
                                             s, t, &texel1);
                if (!ok0 || !ok1) {
                    live_mask &= (uint16_t)~(1u << lane);
                    continue;
                }
                if (color->needs_texel0) store_texel(packet.texel0, lane, texel0);
                if (color->needs_texel1) store_texel(packet.texel1, lane, texel1);
                if (!color->needs_texel1 && color->needs_texel0)
                    store_texel(packet.texel1, lane, texel0);
                if (!color->needs_texel0 && color->needs_texel1)
                    store_texel(packet.texel0, lane, texel1);
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
