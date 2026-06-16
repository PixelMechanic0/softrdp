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

static rdp_color shade_interpolated_color(const raster_decoded_triangle *decoded, int x, int y, int origin_x, int origin_y)
{
    const int64_t dx = (int64_t)(x - origin_x);
    const int64_t dy = (int64_t)(y - origin_y);
    raster_shade_setup shade = decoded->shade;

    shade.r = (int32_t)((int64_t)shade.r + (int64_t)shade.drdx * dx + (int64_t)shade.drdy * dy);
    shade.g = (int32_t)((int64_t)shade.g + (int64_t)shade.dgdx * dx + (int64_t)shade.dgdy * dy);
    shade.b = (int32_t)((int64_t)shade.b + (int64_t)shade.dbdx * dx + (int64_t)shade.dbdy * dy);
    shade.a = (int32_t)((int64_t)shade.a + (int64_t)shade.dadx * dx + (int64_t)shade.dady * dy);

    return shade_base_color(&shade);
}

static int32_t texture_interpolated_value(int32_t base, int32_t ddx, int32_t ddy, int x, int y, int origin_x, int origin_y)
{
    return (int32_t)((int64_t)base + (int64_t)ddx * (int64_t)(x - origin_x) + (int64_t)ddy * (int64_t)(y - origin_y));
}

static int32_t perspective_divide_coord(int32_t coord, int32_t w)
{
    if (w <= 0) return 0;
    int64_t divided = ((int64_t)coord << 16) / (int64_t)w;
    return divided < 0 ? 0 : (divided > INT32_MAX ? INT32_MAX : (int32_t)divided);
}

static uint32_t texture_coord_to_texel(int32_t value)
{
    return value <= 0 ? 0 : (uint32_t)value >> 16;
}

static uint16_t depth_interpolated_value(const raster_decoded_triangle *decoded, int x, int y, int origin_x, int origin_y)
{
    const int64_t value = (int64_t)decoded->depth.z +
                          (int64_t)decoded->depth.dzdx * (int64_t)(x - origin_x) +
                          (int64_t)decoded->depth.dzdy * (int64_t)(y - origin_y);
    return value <= 0 ? 0 : (value >= 0xffff0000ll ? 0xffffu : (uint16_t)((uint64_t)value >> 16));
}

static sr_result depth_test_and_update(sr_memory *memory,
                                       const rdp_state *state,
                                       const raster_decoded_triangle *decoded,
                                       int x,
                                       int y,
                                       int origin_x,
                                       int origin_y,
                                       bool *visible)
{
    *visible = true;
    if (!decoded->has_depth || state->depth_image_address == 0) {
        return SR_OK;
    }

    uint16_t old_depth = 0xffffu;
    const uint16_t new_depth = depth_interpolated_value(decoded, x, y, origin_x, origin_y);
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

static bool triangle_pixel_color(const raster_decoded_triangle *decoded,
                                 const tmem_state *tmem,
                                 const rdp_state *state,
                                 int x,
                                 int y,
                                 int origin_x,
                                 int origin_y,
                                 const rdp_tile_bounds *bounds,
                                 rdp_color *color)
{
    if (decoded->has_texture) {
        const bool needs_texel0 = state && state->combiner_needs_texel0;
        if (needs_texel0) {
            uint16_t texel;
            int32_t s_fixed = texture_interpolated_value(decoded->texture.s, decoded->texture.dsdx, decoded->texture.dsdy, x, y, origin_x, origin_y);
            int32_t t_fixed = texture_interpolated_value(decoded->texture.t, decoded->texture.dtdx, decoded->texture.dtdy, x, y, origin_x, origin_y);

            if (state->other_modes.perspective) {
                const int32_t w_fixed = texture_interpolated_value(decoded->texture.w, decoded->texture.dwdx, decoded->texture.dwdy, x, y, origin_x, origin_y);
                s_fixed = perspective_divide_coord(s_fixed, w_fixed);
                t_fixed = perspective_divide_coord(t_fixed, w_fixed);
            }

            const uint32_t s = texture_coord_to_texel(s_fixed);
            const uint32_t t = texture_coord_to_texel(t_fixed);

            if (tmem_sample_rgba5551(tmem, state, decoded->position.tile & 7u, s, t, bounds, &texel)) {
                rdp_color texel0 = pipeline_rgba5551_to_color(texel);
                rdp_color shade = {0, 0, 0, 255};
                
                const bool needs_shade = state->combiner_needs_shade;
                if (decoded->has_shade && needs_shade) {
                    shade = shade_interpolated_color(decoded, x, y, origin_x, origin_y);
                }
                
                const pipeline_inputs inputs = { .shade = shade, .texel0 = texel0, .primitive = state->primitive_color };
                *color = pipeline_shade_pixel(state, &inputs).color;
                return true;
            }
        } else {
            *color = state ? state->primitive_color : (rdp_color){0, 0, 0, 255};
            return true;
        }
    }

    if (decoded->has_shade) {
        *color = shade_interpolated_color(decoded, x, y, origin_x, origin_y);
        return true;
    }

    return false;
}

sr_result pipeline_process_triangle_pixel(sr_memory *memory,
                                          tmem_state *tmem,
                                          const rdp_state *state,
                                          const raster_decoded_triangle *decoded,
                                          int x, int y,
                                          int origin_x, int origin_y,
                                          bool fill_mode,
                                          const rdp_tile_bounds *bounds)
{
    bool visible = true;
    if (decoded->has_depth && state->depth_image_address != 0) {
        sr_result result = depth_test_and_update(memory, state, decoded, x, y, origin_x, origin_y, &visible);
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
        if (triangle_pixel_color(decoded, tmem, state, x, y, origin_x, origin_y, bounds, &color)) {
            return framebuffer_write_color(memory, state, (uint32_t)x, (uint32_t)y, color);
        }
    }
    return SR_OK;
}

sr_result pipeline_process_rect_pixel(sr_memory *memory,
                                      tmem_state *tmem,
                                      const rdp_state *state,
                                      uint32_t tile_index,
                                      uint32_t s, uint32_t t,
                                      uint32_t x, uint32_t y,
                                      const rdp_tile_bounds *bounds)
{
    if (state && state->simple_combiner == RDP_SIMPLE_COMBINER_PRIMITIVE) {
        return framebuffer_write_color(memory, state, x, y, state->primitive_color);
    }

    uint16_t texel;
    if (!tmem_sample_rgba5551(tmem, state, tile_index, s, t, bounds, &texel)) {
        return SR_ERROR_INVALID_ARGUMENT;
    }

    pipeline_inputs inputs = {
        .texel0 = pipeline_rgba5551_to_color(texel),
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
