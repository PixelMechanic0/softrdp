#include "raster.h"

#include "framebuffer.h"
#include "pipeline.h"
#include "rdp_commands.h"
#include "tmem.h"

#include <stdint.h>

static int32_t sign_extend(uint32_t value, unsigned bits)
{
    const uint32_t mask = 1u << (bits - 1u);
    return (int32_t)((value ^ mask) - mask);
}

static raster_triangle_setup decode_triangle_setup(const rdp_command *cmd)
{
    const uint32_t *w = cmd->words;
    raster_triangle_setup setup;

    setup.flip = (w[0] & 0x00800000u) != 0;
    setup.tile = (uint8_t)((w[0] >> 16) & 0x3fu);
    setup.yl = (int16_t)sign_extend(w[0] & 0x3fffu, 14);
    setup.ym = (int16_t)sign_extend((w[1] >> 16) & 0x3fffu, 14);
    setup.yh = (int16_t)sign_extend(w[1] & 0x3fffu, 14);
    setup.xl = sign_extend(w[2] & 0x0fffffffu, 28);
    setup.dxldy = sign_extend((w[3] >> 2) & 0x0fffffffu, 28);
    setup.xh = sign_extend(w[4] & 0x0fffffffu, 28);
    setup.dxhdy = sign_extend((w[5] >> 2) & 0x0fffffffu, 28);
    setup.xm = sign_extend(w[6] & 0x0fffffffu, 28);
    setup.dxmdy = sign_extend((w[7] >> 2) & 0x0fffffffu, 28);

    return setup;
}

static int32_t join_hi_lo(uint32_t hi_word, unsigned hi_shift, uint32_t lo_word, unsigned lo_shift)
{
    return (int32_t)(((hi_word >> hi_shift) & 0xffff0000u) |
                     ((lo_word >> lo_shift) & 0x0000ffffu));
}

static raster_shade_setup decode_shade_setup(const uint32_t *w)
{
    raster_shade_setup setup;

    setup.r = join_hi_lo(w[0], 0, w[4], 16);
    setup.g = (int32_t)((w[0] << 16) | (w[4] & 0xffffu));
    setup.b = join_hi_lo(w[1], 0, w[5], 16);
    setup.a = (int32_t)((w[1] << 16) | (w[5] & 0xffffu));

    setup.drdx = join_hi_lo(w[2], 0, w[6], 16);
    setup.dgdx = (int32_t)((w[2] << 16) | (w[6] & 0xffffu));
    setup.dbdx = join_hi_lo(w[3], 0, w[7], 16);
    setup.dadx = (int32_t)((w[3] << 16) | (w[7] & 0xffffu));

    setup.drde = join_hi_lo(w[8], 0, w[12], 16);
    setup.dgde = (int32_t)((w[8] << 16) | (w[12] & 0xffffu));
    setup.dbde = join_hi_lo(w[9], 0, w[13], 16);
    setup.dade = (int32_t)((w[9] << 16) | (w[13] & 0xffffu));

    setup.drdy = join_hi_lo(w[10], 0, w[14], 16);
    setup.dgdy = (int32_t)((w[10] << 16) | (w[14] & 0xffffu));
    setup.dbdy = join_hi_lo(w[11], 0, w[15], 16);
    setup.dady = (int32_t)((w[11] << 16) | (w[15] & 0xffffu));

    return setup;
}

static raster_texture_setup decode_texture_setup(const uint32_t *w)
{
    raster_texture_setup setup;

    setup.s = join_hi_lo(w[0], 0, w[4], 16);
    setup.t = (int32_t)((w[0] << 16) | (w[4] & 0xffffu));
    setup.w = join_hi_lo(w[1], 0, w[5], 16);

    setup.dsdx = join_hi_lo(w[2], 0, w[6], 16);
    setup.dtdx = (int32_t)((w[2] << 16) | (w[6] & 0xffffu));
    setup.dwdx = join_hi_lo(w[3], 0, w[7], 16);

    setup.dsde = join_hi_lo(w[8], 0, w[12], 16);
    setup.dtde = (int32_t)((w[8] << 16) | (w[12] & 0xffffu));
    setup.dwde = join_hi_lo(w[9], 0, w[13], 16);

    setup.dsdy = join_hi_lo(w[10], 0, w[14], 16);
    setup.dtdy = (int32_t)((w[10] << 16) | (w[14] & 0xffffu));
    setup.dwdy = join_hi_lo(w[11], 0, w[15], 16);

    return setup;
}

static raster_depth_setup decode_depth_setup(const uint32_t *w)
{
    raster_depth_setup setup;

    setup.z = (int32_t)w[0];
    setup.dzdx = (int32_t)w[1];
    setup.dzde = (int32_t)w[2];
    setup.dzdy = (int32_t)w[3];
    return setup;
}

static uint8_t shade_component_to_u8(int32_t value)
{
    int32_t component = value >> 16;
    if (component < 0) {
        return 0;
    }
    if (component > 255) {
        return 255;
    }
    return (uint8_t)component;
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
    const int64_t dx = (int64_t)(x - origin_x);
    const int64_t dy = (int64_t)(y - origin_y);
    return (int32_t)((int64_t)base + (int64_t)ddx * dx + (int64_t)ddy * dy);
}

static int32_t perspective_divide_coord(int32_t coord, int32_t w)
{
    int64_t divided;

    if (w <= 0) {
        return 0;
    }

    divided = ((int64_t)coord << 16) / (int64_t)w;
    if (divided < 0) {
        return 0;
    }
    if (divided > INT32_MAX) {
        return INT32_MAX;
    }
    return (int32_t)divided;
}

static uint32_t texture_coord_to_texel(int32_t value)
{
    if (value <= 0) {
        return 0;
    }
    return (uint32_t)value >> 16;
}

static uint16_t depth_interpolated_value(const raster_decoded_triangle *decoded, int x, int y, int origin_x, int origin_y)
{
    const int64_t dx = (int64_t)(x - origin_x);
    const int64_t dy = (int64_t)(y - origin_y);
    const int64_t value = (int64_t)decoded->depth.z +
                          (int64_t)decoded->depth.dzdx * dx +
                          (int64_t)decoded->depth.dzdy * dy;

    if (value <= 0) {
        return 0;
    }
    if (value >= 0xffff0000ll) {
        return 0xffffu;
    }
    return (uint16_t)((uint64_t)value >> 16);
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
    uint16_t old_depth = 0xffffu;
    const uint16_t new_depth = depth_interpolated_value(decoded, x, y, origin_x, origin_y);
    const uint32_t pixel = (uint32_t)y * state->color_image.width + (uint32_t)x;
    const uint32_t addr = state->depth_image_address + pixel * 2u;

    *visible = true;
    if (!decoded->has_depth || state->depth_image_address == 0) {
        return SR_OK;
    }

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

static int fixed_floor_div(int64_t value, int64_t scale)
{
    int64_t quotient = value / scale;
    const int64_t remainder = value % scale;

    if (remainder && value < 0) {
        quotient--;
    }
    return (int)quotient;
}

static int fixed_ceil_div(int64_t value, int64_t scale)
{
    return -fixed_floor_div(-value, scale);
}

static int fixed_floor_2(int32_t value)
{
    return fixed_floor_div(value, 4);
}

static int fixed_ceil_16(int64_t value)
{
    return fixed_ceil_div(value, 0x10000);
}

static int scissor_min_x(const rdp_state *state)
{
    return state->scissor_x0 ? (int)((state->scissor_x0 + 3u) >> 2) : 0;
}

static int scissor_min_y(const rdp_state *state)
{
    return state->scissor_y0 ? (int)((state->scissor_y0 + 3u) >> 2) : 0;
}

static int scissor_max_x(const rdp_state *state)
{
    return state->scissor_x1 ? (int)((state->scissor_x1 - 1u) >> 2) : 0x3ff;
}

static int scissor_max_y(const rdp_state *state)
{
    return state->scissor_y1 ? (int)((state->scissor_y1 - 1u) >> 2) : 0x3ff;
}

typedef struct raster_span {
    int y;
    int x0;
    int x1;
} raster_span;

static bool triangle_span_for_y(const raster_triangle_setup *setup, int y, raster_span *span)
{
    int64_t xa;
    int64_t xb;
    const int yh = fixed_floor_2(setup->yh);
    const int ym = fixed_floor_2(setup->ym);

    xa = (int64_t)setup->xh + (int64_t)setup->dxhdy * (int64_t)(y - yh);
    if (y < ym) {
        xb = (int64_t)setup->xm + (int64_t)setup->dxmdy * (int64_t)(y - yh);
    } else {
        xb = (int64_t)setup->xl + (int64_t)setup->dxldy * (int64_t)(y - ym);
    }

    if (xb < xa) {
        const int64_t tmp = xa;
        xa = xb;
        xb = tmp;
    }

    span->y = y;
    span->x0 = fixed_ceil_16(xa);
    span->x1 = fixed_ceil_16(xb) - 1;
    return span->x0 <= span->x1;
}

static bool clip_span_to_scissor(raster_span *span, const rdp_state *state)
{
    const int min_y = scissor_min_y(state);
    const int max_y = scissor_max_y(state);
    const int min_x = scissor_min_x(state);
    const int max_x = scissor_max_x(state);

    if (span->y < min_y || span->y > max_y) {
        return false;
    }
    if (span->x0 < min_x) {
        span->x0 = min_x;
    }
    if (span->x1 > max_x) {
        span->x1 = max_x;
    }
    return span->x0 <= span->x1;
}

static bool triangle_pixel_color(const raster_decoded_triangle *decoded,
                                 const tmem_state *tmem,
                                 const rdp_state *state,
                                 int x,
                                 int y,
                                 int origin_x,
                                 int origin_y,
                                 rdp_color *color)
{
    rdp_color shade = {0, 0, 0, 255};
    rdp_color texel0 = {0, 0, 0, 0};
    bool have_texel = false;

    if (decoded->has_shade) {
        shade = shade_interpolated_color(decoded, x, y, origin_x, origin_y);
    }

    if (decoded->has_texture) {
        uint16_t texel;
        int32_t s_fixed = texture_interpolated_value(decoded->texture.s,
                                                     decoded->texture.dsdx,
                                                     decoded->texture.dsdy,
                                                     x,
                                                     y,
                                                     origin_x,
                                                     origin_y);
        int32_t t_fixed = texture_interpolated_value(decoded->texture.t,
                                                     decoded->texture.dtdx,
                                                     decoded->texture.dtdy,
                                                     x,
                                                     y,
                                                     origin_x,
                                                     origin_y);

        if (state->other_modes.perspective) {
            const int32_t w_fixed = texture_interpolated_value(decoded->texture.w,
                                                               decoded->texture.dwdx,
                                                               decoded->texture.dwdy,
                                                               x,
                                                               y,
                                                               origin_x,
                                                               origin_y);
            s_fixed = perspective_divide_coord(s_fixed, w_fixed);
            t_fixed = perspective_divide_coord(t_fixed, w_fixed);
        }

        const uint32_t s = texture_coord_to_texel(s_fixed);
        const uint32_t t = texture_coord_to_texel(t_fixed);

        if (tmem_sample_rgba5551(tmem, state, decoded->position.tile & 7u, s, t, &texel)) {
            texel0 = pipeline_rgba5551_to_color(texel);
            have_texel = true;
        }
    }

    if (have_texel) {
        const pipeline_inputs inputs = {
            .shade = shade,
            .texel0 = texel0,
            .primitive = state->primitive_color
        };
        *color = pipeline_shade_pixel(state, &inputs).color;
        return true;
    }

    if (decoded->has_shade) {
        *color = shade;
        return true;
    }

    return false;
}

sr_result raster_decode_triangle(const rdp_command *cmd, raster_decoded_triangle *out)
{
    if (!cmd || !out) {
        return SR_ERROR_INVALID_ARGUMENT;
    }

    *out = (raster_decoded_triangle){0};
    out->position = decode_triangle_setup(cmd);

    switch (cmd->id) {
    case RDP_CMD_FILL_TRIANGLE:
        return SR_OK;

    case RDP_CMD_FILL_ZBUFFER_TRIANGLE:
        out->has_depth = true;
        out->depth = decode_depth_setup(&cmd->words[8]);
        return SR_OK;

    case RDP_CMD_TEXTURE_TRIANGLE:
        out->has_texture = true;
        out->texture = decode_texture_setup(&cmd->words[8]);
        return SR_OK;

    case RDP_CMD_TEXTURE_ZBUFFER_TRIANGLE:
        out->has_texture = true;
        out->has_depth = true;
        out->texture = decode_texture_setup(&cmd->words[8]);
        out->depth = decode_depth_setup(&cmd->words[24]);
        return SR_OK;

    case RDP_CMD_SHADE_TRIANGLE:
        out->has_shade = true;
        out->shade = decode_shade_setup(&cmd->words[8]);
        return SR_OK;

    case RDP_CMD_SHADE_ZBUFFER_TRIANGLE:
        out->has_shade = true;
        out->has_depth = true;
        out->shade = decode_shade_setup(&cmd->words[8]);
        out->depth = decode_depth_setup(&cmd->words[24]);
        return SR_OK;

    case RDP_CMD_SHADE_TEXTURE_TRIANGLE:
        out->has_shade = true;
        out->has_texture = true;
        out->shade = decode_shade_setup(&cmd->words[8]);
        out->texture = decode_texture_setup(&cmd->words[24]);
        return SR_OK;

    case RDP_CMD_SHADE_TEXTURE_ZBUFFER_TRIANGLE:
        out->has_shade = true;
        out->has_texture = true;
        out->has_depth = true;
        out->shade = decode_shade_setup(&cmd->words[8]);
        out->texture = decode_texture_setup(&cmd->words[24]);
        out->depth = decode_depth_setup(&cmd->words[40]);
        return SR_OK;

    default:
        return SR_ERROR_BAD_COMMAND;
    }
}

typedef struct raster_rect {
    uint32_t x0;
    uint32_t y0;
    uint32_t x1;
    uint32_t y1;
} raster_rect;

static raster_rect decode_command_rect(const rdp_command *cmd)
{
    raster_rect rect;

    rect.x1 = ((cmd->words[0] >> 12) & 0xfffu) >> 2;
    rect.y1 = (cmd->words[0] & 0xfffu) >> 2;
    rect.x0 = ((cmd->words[1] >> 12) & 0xfffu) >> 2;
    rect.y0 = (cmd->words[1] & 0xfffu) >> 2;
    return rect;
}

static bool clip_rect_to_scissor(raster_rect *rect, const rdp_state *state)
{
    if (state->scissor_x0 > rect->x0 << 2) {
        rect->x0 = (state->scissor_x0 + 3u) >> 2;
    }
    if (state->scissor_y0 > rect->y0 << 2) {
        rect->y0 = (state->scissor_y0 + 3u) >> 2;
    }
    if (state->scissor_x1 && state->scissor_x1 <= rect->x1 << 2) {
        rect->x1 = (state->scissor_x1 - 1u) >> 2;
    }
    if (state->scissor_y1 && state->scissor_y1 <= rect->y1 << 2) {
        rect->y1 = (state->scissor_y1 - 1u) >> 2;
    }

    return rect->x0 <= rect->x1 && rect->y0 <= rect->y1;
}

sr_result raster_submit_triangle(sr_memory *memory, tmem_state *tmem, rdp_state *state, const rdp_command *cmd)
{
    raster_decoded_triangle decoded;
    sr_result result = raster_decode_triangle(cmd, &decoded);
    if (result != SR_OK) {
        return result;
    }
    if (cmd->id != RDP_CMD_FILL_TRIANGLE && !decoded.has_shade && !decoded.has_texture) {
        return SR_OK;
    }

    const int yh = fixed_floor_2(decoded.position.yh);
    const int yl = fixed_floor_2(decoded.position.yl);
    const bool fill_triangle = cmd->id == RDP_CMD_FILL_TRIANGLE;
    const int shade_origin_x = fixed_ceil_16(decoded.position.xh);
    const int shade_origin_y = yh;

    if (yl <= yh) {
        return SR_OK;
    }

    for (int y = yh; y < yl; y++) {
        raster_span span;
        if (!triangle_span_for_y(&decoded.position, y, &span)) {
            continue;
        }
        if (!clip_span_to_scissor(&span, state)) {
            continue;
        }

        for (int x = span.x0; x <= span.x1; x++) {
            bool visible;
            if (x < 0 || y < 0) {
                continue;
            }

            result = depth_test_and_update(memory, state, &decoded, x, y, shade_origin_x, shade_origin_y, &visible);
            if (result != SR_OK) {
                return result;
            }
            if (!visible) {
                continue;
            }

            if (fill_triangle) {
                result = framebuffer_write_fill_pixel(memory, state, (uint32_t)x, (uint32_t)y);
            } else {
                rdp_color color;
                if (!triangle_pixel_color(&decoded, tmem, state, x, y, shade_origin_x, shade_origin_y, &color)) {
                    continue;
                }
                result = framebuffer_write_color(memory, state, (uint32_t)x, (uint32_t)y, color);
            }
            if (result != SR_OK) {
                return result;
            }
        }
    }

    return SR_OK;
}

static sr_result submit_texture_rectangle(sr_memory *memory, tmem_state *tmem, rdp_state *state, const rdp_command *cmd)
{
    const uint32_t tile_index = (cmd->words[1] >> 24) & 7u;
    const int32_t s0 = (int16_t)(cmd->words[2] >> 16);
    const int32_t t0 = (int16_t)cmd->words[2];
    const int32_t dsdx = (int16_t)(cmd->words[3] >> 16);
    const int32_t dtdy = (int16_t)cmd->words[3];
    const bool flip = cmd->id == RDP_CMD_TEXTURE_RECTANGLE_FLIP;
    raster_rect rect = decode_command_rect(cmd);
    const uint32_t base_x = rect.x0;
    const uint32_t base_y = rect.y0;

    if (!clip_rect_to_scissor(&rect, state)) {
        return SR_OK;
    }

    for (uint32_t y = rect.y0; y <= rect.y1; y++) {
        for (uint32_t x = rect.x0; x <= rect.x1; x++) {
            const int32_t dx = (int32_t)(x - base_x);
            const int32_t dy = (int32_t)(y - base_y);
            const int32_t s_fixed = s0 + (flip ? dy * dtdy : dx * dsdx);
            const int32_t t_fixed = t0 + (flip ? dx * dsdx : dy * dtdy);
            const uint32_t s = s_fixed < 0 ? 0u : (uint32_t)s_fixed >> 5;
            const uint32_t t = t_fixed < 0 ? 0u : (uint32_t)t_fixed >> 5;
            uint16_t texel;

            if (!tmem_sample_rgba5551(tmem, state, tile_index, s, t, &texel)) {
                return SR_ERROR_INVALID_ARGUMENT;
            }

            pipeline_inputs inputs = {
                .texel0 = pipeline_rgba5551_to_color(texel),
                .primitive = state->primitive_color
            };
            pipeline_outputs outputs = pipeline_shade_pixel(state, &inputs);
            sr_result result = framebuffer_write_color(memory, state, x, y, outputs.color);
            if (result != SR_OK) {
                return result;
            }
        }
    }

    return SR_OK;
}

sr_result raster_submit_rectangle(sr_memory *memory, tmem_state *tmem, rdp_state *state, const rdp_command *cmd)
{
    raster_rect rect;

    if (cmd->id == RDP_CMD_TEXTURE_RECTANGLE || cmd->id == RDP_CMD_TEXTURE_RECTANGLE_FLIP) {
        return submit_texture_rectangle(memory, tmem, state, cmd);
    }

    if (cmd->id != RDP_CMD_FILL_RECTANGLE) {
        return SR_OK;
    }

    rect = decode_command_rect(cmd);
    if (state->other_modes.cycle_type == RDP_CYCLE_FILL ||
        state->other_modes.cycle_type == RDP_CYCLE_COPY) {
        rect.y1 = ((cmd->words[0] & 0xfffu) | 3u) >> 2;
    }

    if (!clip_rect_to_scissor(&rect, state)) {
        return SR_OK;
    }

    return framebuffer_fill_rect(memory, state, rect.x0, rect.y0, rect.x1, rect.y1);
}
