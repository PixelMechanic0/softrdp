#include "raster.h"

#include "framebuffer.h"
#include "pipeline.h"
#include "rdp_commands.h"
#include "tmem.h"

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
    (void)tmem;
    raster_decoded_triangle decoded;
    sr_result result = raster_decode_triangle(cmd, &decoded);
    if (result != SR_OK || cmd->id != RDP_CMD_FILL_TRIANGLE) {
        return result;
    }

    const int yh = decoded.position.yh >> 2;
    const int ym = decoded.position.ym >> 2;
    const int yl = decoded.position.yl >> 2;
    const int64_t xh = decoded.position.xh;
    const int64_t xm = decoded.position.xm;
    const int64_t xl = decoded.position.xl;
    const int64_t dxhdy = decoded.position.dxhdy;
    const int64_t dxmdy = decoded.position.dxmdy;
    const int64_t dxldy = decoded.position.dxldy;

    if (yl <= yh) {
        return SR_OK;
    }

    for (int y = yh; y < yl; y++) {
        int64_t xa = xh + dxhdy * (int64_t)(y - yh);
        int64_t xb;

        if (y < ym) {
            xb = xm + dxmdy * (int64_t)(y - yh);
        } else {
            xb = xl + dxldy * (int64_t)(y - ym);
        }

        if (xb < xa) {
            int64_t tmp = xa;
            xa = xb;
            xb = tmp;
        }

        int x0 = xa >= 0 ? (int)((xa + 0xffff) >> 16) : (int)(xa / 0x10000);
        int x1 = xb >= 0 ? (int)(((xb + 0xffff) >> 16) - 1) : (int)((xb / 0x10000) - 1);

        if (state->scissor_y0 && y < (int)((state->scissor_y0 + 3u) >> 2)) {
            continue;
        }
        if (state->scissor_y1 && y > (int)((state->scissor_y1 - 1u) >> 2)) {
            continue;
        }

        if (state->scissor_x0 && x0 < (int)((state->scissor_x0 + 3u) >> 2)) {
            x0 = (int)((state->scissor_x0 + 3u) >> 2);
        }
        if (state->scissor_x1 && x1 > (int)((state->scissor_x1 - 1u) >> 2)) {
            x1 = (int)((state->scissor_x1 - 1u) >> 2);
        }

        for (int x = x0; x <= x1; x++) {
            if (x < 0 || y < 0) {
                continue;
            }

            result = framebuffer_write_fill_pixel(memory, state, (uint32_t)x, (uint32_t)y);
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
