#include "pipeline.h"
#include <string.h>
#include "framebuffer.h"
#include "rdp_memory.h"
#include "tmem.h"

static uint8_t multiply_u8(uint8_t a, uint8_t b)
{
    return (uint8_t)(((uint32_t)a * (uint32_t)b + 127u) / 255u);
}

static rdp_color modulate_color(rdp_color texel, rdp_color shade)
{
    return (rdp_color){
        multiply_u8(texel.r, shade.r),
        multiply_u8(texel.g, shade.g),
        multiply_u8(texel.b, shade.b),
        multiply_u8(texel.a, shade.a)
    };
}

pipeline_outputs pipeline_shade_pixel(const rdp_state *state, const pipeline_inputs *inputs)
{
    pipeline_outputs out = { .coverage = 7 };
    if (!inputs) out.color = (rdp_color){0, 0, 0, 255};
    else if (state && state->simple_combiner == RDP_SIMPLE_COMBINER_PRIMITIVE) out.color = inputs->primitive;
    else if (state && state->simple_combiner == RDP_SIMPLE_COMBINER_TEXEL0_SHADE) out.color = modulate_color(inputs->texel0, inputs->shade);
    else out.color = inputs->texel0;
    return out;
}

/* Moved Fragment Processing Helper Functions */

static uint8_t shade_component_to_u8(int32_t value)
{
    int32_t component = value >> 16;
    return component < 0 ? 0 : (component > 255 ? 255 : (uint8_t)component);
}

static rdp_color shade_base_color(const raster_shade_setup *shade)
{
    return (rdp_color){
        shade_component_to_u8(shade->r),
        shade_component_to_u8(shade->g),
        shade_component_to_u8(shade->b),
        shade_component_to_u8(shade->a)
    };
}

static int32_t interpolate_attribute(const raster_decoded_triangle *decoded,
                                     int32_t base,
                                     int32_t ddx,
                                     int32_t dde,
                                     int32_t ddy,
                                     int x,
                                     int y)
{
    const bool sign_dxhdy = decoded->position.dxhdy < 0;
    const bool do_offset = sign_dxhdy == decoded->position.flip;
    const int y_base = decoded->position.yh >> 2;
    const int dy = y - y_base;
    int64_t xh = (int64_t)decoded->position.xh + ((int64_t)dy * (int64_t)decoded->position.dxhdy);
    int32_t diff = 0;

    if (do_offset) {
        const int32_t ddeh = dde & ~0x1ff;
        const int32_t ddyh = ddy & ~0x1ff;
        xh += (int64_t)3 * (int64_t)decoded->position.dxhdy / 4;
        diff = ddeh - (ddeh >> 2) - ddyh + (ddyh >> 2);
    }

    const int base_x = (int)(xh >> 16);
    const int xfrac = (int)((xh >> 8) & 0xff);
    int64_t value = (int64_t)base + (int64_t)dde * dy;
    value = ((value & ~0x1ffll) + diff - (int64_t)xfrac * ((ddx >> 8) & ~1)) & ~0x3ffll;
    value += (int64_t)(ddx & ~0x1f) * (int64_t)(x - base_x);
    if (value < INT32_MIN) return INT32_MIN;
    if (value > INT32_MAX) return INT32_MAX;
    return (int32_t)value;
}

static rdp_color shade_interpolated_color(const raster_decoded_triangle *decoded, int x, int y)
{
    raster_shade_setup shade = decoded->shade;

    shade.r = interpolate_attribute(decoded, shade.r, shade.drdx, shade.drde, shade.drdy, x, y);
    shade.g = interpolate_attribute(decoded, shade.g, shade.dgdx, shade.dgde, shade.dgdy, x, y);
    shade.b = interpolate_attribute(decoded, shade.b, shade.dbdx, shade.dbde, shade.dbdy, x, y);
    shade.a = interpolate_attribute(decoded, shade.a, shade.dadx, shade.dade, shade.dady, x, y);

    return shade_base_color(&shade);
}

static int32_t texture_interpolated_value(int32_t base, int32_t ddx, int32_t ddy, int64_t dx_fixed, int64_t dy_fixed)
{
    return (int32_t)((int64_t)base + ((int64_t)ddx * dx_fixed + (int64_t)ddy * dy_fixed) / 65536);
}

static int32_t perspective_divide_coord(int32_t coord, int32_t w)
{
    if (w <= 0) return 0;
    int64_t divided = ((int64_t)coord << 16) / (int64_t)w;
    return divided < 0 ? 0 : (divided > INT32_MAX ? INT32_MAX : (int32_t)divided);
}

static uint32_t texture_coord_to_texel(int32_t value)
{
    return value <= 0 ? 0 : (uint32_t)value >> 5;
}

static uint16_t depth_interpolated_value(const raster_decoded_triangle *decoded, int64_t dx_fixed, int64_t dy_fixed)
{
    const int64_t value = (int64_t)decoded->depth.z +
                          ((int64_t)decoded->depth.dzdx * dx_fixed + (int64_t)decoded->depth.dzdy * dy_fixed) / 65536;
    return value <= 0 ? 0 : (value >= 0xffff0000ll ? 0xffffu : (uint16_t)((uint64_t)value >> 16));
}

static uint16_t depth_fixed_to_u16(int64_t value)
{
    return value <= 0 ? 0 : (value >= 0xffff0000ll ? 0xffffu : (uint16_t)((uint64_t)value >> 16));
}

static void record_texture_sample_coord(rdp_state *state, uint32_t s, uint32_t t)
{
    if (!state) {
        return;
    }

    if (state->texture_sample_attempts == 0) {
        state->texture_sample_min_s = s;
        state->texture_sample_max_s = s;
        state->texture_sample_min_t = t;
        state->texture_sample_max_t = t;
    } else {
        if (s < state->texture_sample_min_s) state->texture_sample_min_s = s;
        if (s > state->texture_sample_max_s) state->texture_sample_max_s = s;
        if (t < state->texture_sample_min_t) state->texture_sample_min_t = t;
        if (t > state->texture_sample_max_t) state->texture_sample_max_t = t;
    }
}

static void record_texture_sample_fixed(rdp_state *state, int32_t s, int32_t t, int32_t w)
{
    if (!state) {
        return;
    }

    if (state->texture_sample_attempts == 0) {
        state->texture_sample_min_s_fixed = s;
        state->texture_sample_max_s_fixed = s;
        state->texture_sample_min_t_fixed = t;
        state->texture_sample_max_t_fixed = t;
        state->texture_sample_min_w_fixed = w;
        state->texture_sample_max_w_fixed = w;
    } else {
        if (s < state->texture_sample_min_s_fixed) state->texture_sample_min_s_fixed = s;
        if (s > state->texture_sample_max_s_fixed) state->texture_sample_max_s_fixed = s;
        if (t < state->texture_sample_min_t_fixed) state->texture_sample_min_t_fixed = t;
        if (t > state->texture_sample_max_t_fixed) state->texture_sample_max_t_fixed = t;
        if (w < state->texture_sample_min_w_fixed) state->texture_sample_min_w_fixed = w;
        if (w > state->texture_sample_max_w_fixed) state->texture_sample_max_w_fixed = w;
    }
}

static void record_texture_sample_color(rdp_state *state, rdp_color color)
{
    if (!state) {
        return;
    }

    const uint32_t packed = ((uint32_t)color.r << 24) |
                            ((uint32_t)color.g << 16) |
                            ((uint32_t)color.b << 8) |
                            (uint32_t)color.a;
    state->texture_sample_color_xor ^= packed;
}

static void record_texture_sample_attempt(rdp_state *state, const rdp_tile *tile)
{
    if (!state || !tile) {
        return;
    }

    state->texture_sample_attempts++;
    if (tile->format <= RDP_FORMAT_I && tile->size <= RDP_SIZE_32BPP) {
        state->texture_sample_by_format_size[tile->format][tile->size]++;
    }
    if (state->other_modes.tlut_enable) {
        state->texture_sample_tlut_attempts++;
    }
    if (state->other_modes.bilerp0) {
        state->texture_sample_bilerp_attempts++;
    }
    if (state->other_modes.sample_quad) {
        state->texture_sample_quad_attempts++;
    }
    if (state->other_modes.mid_texel) {
        state->texture_sample_mid_texel_attempts++;
    }
    if (state->other_modes.perspective) {
        state->texture_sample_perspective_attempts++;
    }
    if (state->simple_combiner == RDP_SIMPLE_COMBINER_TEXEL0_SHADE) {
        state->texture_sample_texel0_shade_attempts++;
    }
}

static void record_texture_sample_hit(rdp_state *state, const rdp_tile *tile)
{
    if (!state || !tile) {
        return;
    }

    state->texture_sample_hits++;
    if (tile->format <= RDP_FORMAT_I && tile->size <= RDP_SIZE_32BPP) {
        state->texture_sample_hits_by_format_size[tile->format][tile->size]++;
    }
}

static sr_result depth_test_and_update(sr_memory *memory,
                                       const rdp_state *state,
                                       const raster_decoded_triangle *decoded,
                                       int x,
                                       int y,
                                       int64_t dx_fixed,
                                       int64_t dy_fixed,
                                       bool *visible)
{
    *visible = true;
    if (!decoded->has_depth || state->depth_image_address == 0) {
        return SR_OK;
    }

    uint16_t old_depth = 0xffffu;
    const uint16_t new_depth = depth_interpolated_value(decoded, dx_fixed, dy_fixed);
    const uint32_t pixel = (uint32_t)y * state->color_image.width + (uint32_t)x;
    const uint32_t addr = state->depth_image_address + pixel * 2u;

    if ((state->other_modes.z_compare || state->other_modes.z_update) &&
        !sr_memory_read_be16(memory, addr, &old_depth)) {
        return SR_ERROR_INVALID_ARGUMENT;
    }

    if (state->other_modes.z_compare && new_depth > old_depth) {
        *visible = false;
        return SR_OK;
    }

    if (state->other_modes.z_update &&
        !sr_memory_write_be16(memory, addr, new_depth)) {
        return SR_ERROR_INVALID_ARGUMENT;
    }

    return SR_OK;
}

typedef struct triangle_span_state {
    int64_t depth_fixed;
    int32_t s_fixed;
    int32_t t_fixed;
    int32_t w_fixed;
    raster_shade_setup shade;
} triangle_span_state;

static void triangle_span_state_init(const raster_decoded_triangle *decoded,
                                     const rdp_state *state,
                                     int x,
                                     int y,
                                     int64_t dx_fixed,
                                     int64_t dy_fixed,
                                     triangle_span_state *span)
{
    memset(span, 0, sizeof(*span));

    if (decoded->has_depth) {
        span->depth_fixed = (int64_t)decoded->depth.z +
                            ((int64_t)decoded->depth.dzdx * dx_fixed +
                             (int64_t)decoded->depth.dzdy * dy_fixed) / 65536;
    }

    if (decoded->has_texture && state && state->combiner_needs_texel0) {
        span->s_fixed = texture_interpolated_value(decoded->texture.s, decoded->texture.dsdx, decoded->texture.dsdy, dx_fixed, dy_fixed);
        span->t_fixed = texture_interpolated_value(decoded->texture.t, decoded->texture.dtdx, decoded->texture.dtdy, dx_fixed, dy_fixed);
        if (state->other_modes.perspective) {
            span->w_fixed = texture_interpolated_value(decoded->texture.w, decoded->texture.dwdx, decoded->texture.dwdy, dx_fixed, dy_fixed);
        }
    }

    if (decoded->has_shade) {
        span->shade = decoded->shade;
        span->shade.r = interpolate_attribute(decoded, span->shade.r, span->shade.drdx, span->shade.drde, span->shade.drdy, x, y);
        span->shade.g = interpolate_attribute(decoded, span->shade.g, span->shade.dgdx, span->shade.dgde, span->shade.dgdy, x, y);
        span->shade.b = interpolate_attribute(decoded, span->shade.b, span->shade.dbdx, span->shade.dbde, span->shade.dbdy, x, y);
        span->shade.a = interpolate_attribute(decoded, span->shade.a, span->shade.dadx, span->shade.dade, span->shade.dady, x, y);
    }
}

static void triangle_span_state_step(const raster_decoded_triangle *decoded,
                                     const rdp_state *state,
                                     triangle_span_state *span)
{
    if (decoded->has_depth) {
        span->depth_fixed += decoded->depth.dzdx;
    }
    if (decoded->has_texture && state && state->combiner_needs_texel0) {
        span->s_fixed += decoded->texture.dsdx;
        span->t_fixed += decoded->texture.dtdx;
        if (state->other_modes.perspective) {
            span->w_fixed += decoded->texture.dwdx;
        }
    }
    if (decoded->has_shade) {
        span->shade.r += decoded->shade.drdx & ~0x1f;
        span->shade.g += decoded->shade.dgdx & ~0x1f;
        span->shade.b += decoded->shade.dbdx & ~0x1f;
        span->shade.a += decoded->shade.dadx & ~0x1f;
    }
}

static bool triangle_pixel_color(const raster_decoded_triangle *decoded,
                                 const tmem_state *tmem,
                                 rdp_state *state,
                                 int64_t dx_fixed,
                                 int64_t dy_fixed,
                                 int x,
                                 int y,
                                 const rdp_tile_bounds *bounds,
                                 rdp_color *color)
{
    if (decoded->has_texture) {
        const bool needs_texel0 = state && state->combiner_needs_texel0;
        if (needs_texel0) {
            int32_t s_fixed = texture_interpolated_value(decoded->texture.s, decoded->texture.dsdx, decoded->texture.dsdy, dx_fixed, dy_fixed);
            int32_t t_fixed = texture_interpolated_value(decoded->texture.t, decoded->texture.dtdx, decoded->texture.dtdy, dx_fixed, dy_fixed);
            int32_t w_fixed = 0;

            if (state->other_modes.perspective) {
                w_fixed = texture_interpolated_value(decoded->texture.w, decoded->texture.dwdx, decoded->texture.dwdy, dx_fixed, dy_fixed);
                s_fixed = perspective_divide_coord(s_fixed, w_fixed);
                t_fixed = perspective_divide_coord(t_fixed, w_fixed);
            }

            const uint32_t s = texture_coord_to_texel(s_fixed);
            const uint32_t t = texture_coord_to_texel(t_fixed);

            rdp_color texel0;
            record_texture_sample_fixed(state, s_fixed, t_fixed, w_fixed);
            record_texture_sample_coord(state, s, t);
            record_texture_sample_attempt(state, &state->tiles[decoded->position.tile & 7u]);
            if (tmem_sample_color_fixed5(tmem, state, decoded->position.tile & 7u, s_fixed, t_fixed, bounds, &texel0)) {
                record_texture_sample_hit(state, &state->tiles[decoded->position.tile & 7u]);
                record_texture_sample_color(state, texel0);
                rdp_color shade = {0, 0, 0, 255};
                
                const bool needs_shade = state->combiner_needs_shade;
                if (decoded->has_shade && needs_shade) {
                    shade = shade_interpolated_color(decoded, x, y);
                }
                
                const pipeline_inputs inputs = { .shade = shade, .texel0 = texel0, .primitive = state->primitive_color };
                *color = pipeline_shade_pixel(state, &inputs).color;
                return true;
            }
            state->texture_sample_misses++;
        } else {
            *color = state ? state->primitive_color : (rdp_color){0, 0, 0, 255};
            return true;
        }
    }

    if (decoded->has_shade) {
        if (state && decoded->has_texture) {
            state->texture_sample_shade_fallbacks++;
        }
        *color = shade_interpolated_color(decoded, x, y);
        return true;
    }

    return false;
}

sr_result pipeline_process_triangle_pixel(sr_memory *memory,
                                          tmem_state *tmem,
                                          rdp_state *state,
                                          const raster_decoded_triangle *decoded,
                                          int x, int y,
                                          bool fill_mode,
                                          const rdp_tile_bounds *bounds)
{
    // Exact subpixel distance offsets from the top vertex (decoded->position.xh, yh)
    int64_t dx_fixed = ((int64_t)x << 16) + 0x8000 - (int64_t)decoded->position.xh;
    int64_t dy_fixed = (((int64_t)y << 2) + 2 - (int64_t)decoded->position.yh) << 14;

    bool visible = true;
    if (decoded->has_depth && state->depth_image_address != 0) {
        sr_result result = depth_test_and_update(memory, state, decoded, x, y, dx_fixed, dy_fixed, &visible);
        if (result != SR_OK) {
            return result;
        }
        if (!visible) {
            return SR_OK;
        }
    }

    if (fill_mode) {
        return framebuffer_write_fill_pixel(memory, state, (uint32_t)x, (uint32_t)y);
    } else {
        rdp_color color;
        if (triangle_pixel_color(decoded, tmem, state, dx_fixed, dy_fixed, x, y, bounds, &color)) {
            return framebuffer_write_color(memory, state, (uint32_t)x, (uint32_t)y, color);
        }
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
sr_result pipeline_process_triangle_span(sr_memory *memory,
                                         tmem_state *tmem,
                                         rdp_state *state,
                                         const raster_decoded_triangle *decoded,
                                         int x0, int x1, int y,
                                         bool fill_mode,
                                         const rdp_tile_bounds *bounds)
{
    if (x1 < x0) {
        return SR_OK;
    }

    int64_t dx_fixed = ((int64_t)x0 << 16) + 0x8000 - (int64_t)decoded->position.xh;
    int64_t dy_fixed = (((int64_t)y << 2) + 2 - (int64_t)decoded->position.yh) << 14;
    triangle_span_state span;
    triangle_span_state_init(decoded, state, x0, y, dx_fixed, dy_fixed, &span);

    for (int x = x0; x <= x1; x++) {
        bool visible = true;
        if (decoded->has_depth && state->depth_image_address != 0) {
            uint16_t old_depth = 0xffffu;
            const uint16_t new_depth = depth_fixed_to_u16(span.depth_fixed);
            const uint32_t pixel = (uint32_t)y * state->color_image.width + (uint32_t)x;
            const uint32_t addr = state->depth_image_address + pixel * 2u;

            if ((state->other_modes.z_compare || state->other_modes.z_update) &&
                !sr_memory_read_be16(memory, addr, &old_depth)) {
                return SR_ERROR_INVALID_ARGUMENT;
            }
            if (state->other_modes.z_compare && new_depth > old_depth) {
                visible = false;
            } else if (state->other_modes.z_update &&
                       !sr_memory_write_be16(memory, addr, new_depth)) {
                return SR_ERROR_INVALID_ARGUMENT;
            }
        }

        if (visible) {
            if (fill_mode) {
                sr_result result = framebuffer_write_fill_pixel(memory, state, (uint32_t)x, (uint32_t)y);
                if (result != SR_OK) {
                    return result;
                }
            } else if (decoded->has_texture) {
                const bool needs_texel0 = state && state->combiner_needs_texel0;
                bool wrote_color = false;
                if (needs_texel0) {
                    int32_t s_fixed = span.s_fixed;
                    int32_t t_fixed = span.t_fixed;
                    if (state->other_modes.perspective) {
                        const int32_t raw_w_fixed = span.w_fixed;
                        s_fixed = perspective_divide_coord(s_fixed, span.w_fixed);
                        t_fixed = perspective_divide_coord(t_fixed, span.w_fixed);
                        span.w_fixed = raw_w_fixed;
                    }

                    rdp_color texel0;
                    record_texture_sample_fixed(state, s_fixed, t_fixed, state->other_modes.perspective ? span.w_fixed : 0);
                    record_texture_sample_coord(state, texture_coord_to_texel(s_fixed), texture_coord_to_texel(t_fixed));
                    record_texture_sample_attempt(state, &state->tiles[decoded->position.tile & 7u]);
                    if (tmem_sample_color_fixed5(tmem, state, decoded->position.tile & 7u,
                                                 s_fixed,
                                                 t_fixed,
                                                 bounds, &texel0)) {
                        record_texture_sample_hit(state, &state->tiles[decoded->position.tile & 7u]);
                        record_texture_sample_color(state, texel0);
                        rdp_color color;
                        if (decoded->has_shade && state->combiner_needs_shade) {
                            const rdp_color shade = shade_base_color(&span.shade);
                            color = modulate_color(texel0, shade);
                        } else {
                            color = pipeline_shade_pixel(state, &(pipeline_inputs){
                                .texel0 = texel0,
                                .primitive = state->primitive_color
                            }).color;
                        }
                        sr_result result = framebuffer_write_color(memory, state, (uint32_t)x, (uint32_t)y, color);
                        if (result != SR_OK) {
                            return result;
                        }
                        wrote_color = true;
                    } else {
                        state->texture_sample_misses++;
                    }
                } else {
                    sr_result result = framebuffer_write_color(memory, state, (uint32_t)x, (uint32_t)y,
                                                               state ? state->primitive_color : (rdp_color){0, 0, 0, 255});
                    if (result != SR_OK) {
                        return result;
                    }
                    wrote_color = true;
                }

                if (!wrote_color && decoded->has_shade) {
                    if (state) {
                        state->texture_sample_shade_fallbacks++;
                    }
                    sr_result result = framebuffer_write_color(memory, state, (uint32_t)x, (uint32_t)y,
                                                               shade_base_color(&span.shade));
                    if (result != SR_OK) {
                        return result;
                    }
                }
            } else if (decoded->has_shade) {
                sr_result result = framebuffer_write_color(memory, state, (uint32_t)x, (uint32_t)y,
                                                           shade_base_color(&span.shade));
                if (result != SR_OK) {
                    return result;
                }
            }
        }

        triangle_span_state_step(decoded, state, &span);
    }

    return SR_OK;
}

sr_result pipeline_process_rect_pixel(sr_memory *memory,
                                      tmem_state *tmem,
                                      rdp_state *state,
                                      uint32_t tile_index,
                                      uint32_t s, uint32_t t,
                                      uint32_t x, uint32_t y,
                                      const rdp_tile_bounds *bounds)
{
    if (state && state->simple_combiner == RDP_SIMPLE_COMBINER_PRIMITIVE) {
        return framebuffer_write_color(memory, state, x, y, state->primitive_color);
    }

    rdp_color texel0;
    if (state) {
        state->rect_texture_sample_attempts++;
    }
    if (!tmem_sample_color(tmem, state, tile_index, s, t, bounds, &texel0)) {
        if (state) {
            state->rect_texture_sample_misses++;
        }
        return SR_ERROR_INVALID_ARGUMENT;
    }
    if (state) {
        state->rect_texture_sample_hits++;
    }

    pipeline_inputs inputs = {
        .texel0 = texel0,
        .primitive = state->primitive_color
    };
    pipeline_outputs outputs = pipeline_shade_pixel(state, &inputs);
    return framebuffer_write_color(memory, state, x, y, outputs.color);
}

void pipeline_resolve_tile_bounds(const rdp_state *state, const tmem_state *tmem, uint32_t tile_index, rdp_tile_bounds *bounds)
{
    if (!bounds) return;
    if (!state || !tmem || tile_index >= 8) {
        memset(bounds, 0, sizeof(*bounds));
        return;
    }
    const rdp_tile *tile = &state->tiles[tile_index];
    bounds->sl = tmem->tile_sl[tile_index];
    bounds->tl = tmem->tile_tl[tile_index];
    bounds->sh = tmem->tile_sh[tile_index];
    bounds->th = tmem->tile_th[tile_index];
    if (tile->sh > tile->sl || tile->th > tile->tl) {
        bounds->sl = tile->sl >> 2;
        bounds->tl = tile->tl >> 2;
        bounds->sh = tile->sh >> 2;
        bounds->th = tile->th >> 2;
    }
}
