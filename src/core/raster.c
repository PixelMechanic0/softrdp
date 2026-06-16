#include "raster.h"

#include "framebuffer.h"
#include "pipeline.h"

#include <stdint.h>

#if SOFTRDP_ENABLE_PERF_LOG
#define NOMINMAX
#include <windows.h>
#endif

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
    setup.dxldy = sign_extend(w[3] & 0x3fffffffu, 30);
    setup.xh = sign_extend(w[4] & 0x0fffffffu, 28);
    setup.dxhdy = sign_extend(w[5] & 0x3fffffffu, 30);
    setup.xm = sign_extend(w[6] & 0x0fffffffu, 28);
    setup.dxmdy = sign_extend(w[7] & 0x3fffffffu, 30);

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
    int64_t xh;
    int64_t xl;

    // Center of scanline y is y * 4 + 2 subpixels
    int64_t y_center = (int64_t)y * 4 + 2;
    int64_t yh_base = (int64_t)setup->yh & ~3ll;
    int64_t dy_h = y_center - yh_base;

    // Triangle edge slopes are expressed per quarter scanline. Keep the left/right
    // identity from the flip bit; swapping edges breaks valid right-major triangles.
    xh = (int64_t)setup->xh + ((int64_t)setup->dxhdy * dy_h) / 4;
    if (y_center < (int64_t)setup->ym) {
        xl = (int64_t)setup->xm + ((int64_t)setup->dxmdy * dy_h) / 4;
    } else {
        int64_t dy_l = y_center - (int64_t)setup->ym;
        xl = (int64_t)setup->xl + ((int64_t)setup->dxldy * dy_l) / 4;
    }

    span->y = y;
    if (setup->flip) {
        span->x0 = fixed_ceil_16(xh);
        span->x1 = fixed_ceil_16(xl) - 1;
    } else {
        span->x0 = fixed_ceil_16(xl);
        span->x1 = fixed_ceil_16(xh) - 1;
    }
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

sr_result raster_decode_triangle(const rdp_command *cmd, raster_decoded_triangle *out)
{
    if (!cmd || !out) {
        return SR_ERROR_INVALID_ARGUMENT;
    }

    *out = (raster_decoded_triangle){0};
    out->position = decode_triangle_setup(cmd);

    switch (cmd->id) {
    case RDP_CMD_FILL_TRIANGLE:                   return SR_OK;
    case RDP_CMD_FILL_ZBUFFER_TRIANGLE:           out->has_depth = true; out->depth = decode_depth_setup(&cmd->words[8]); return SR_OK;
    case RDP_CMD_TEXTURE_TRIANGLE:                out->has_texture = true; out->texture = decode_texture_setup(&cmd->words[8]); return SR_OK;
    case RDP_CMD_TEXTURE_ZBUFFER_TRIANGLE:        out->has_texture = out->has_depth = true; out->texture = decode_texture_setup(&cmd->words[8]); out->depth = decode_depth_setup(&cmd->words[24]); return SR_OK;
    case RDP_CMD_SHADE_TRIANGLE:                  out->has_shade = true; out->shade = decode_shade_setup(&cmd->words[8]); return SR_OK;
    case RDP_CMD_SHADE_ZBUFFER_TRIANGLE:          out->has_shade = out->has_depth = true; out->shade = decode_shade_setup(&cmd->words[8]); out->depth = decode_depth_setup(&cmd->words[24]); return SR_OK;
    case RDP_CMD_SHADE_TEXTURE_TRIANGLE:          out->has_shade = out->has_texture = true; out->shade = decode_shade_setup(&cmd->words[8]); out->texture = decode_texture_setup(&cmd->words[24]); return SR_OK;
    case RDP_CMD_SHADE_TEXTURE_ZBUFFER_TRIANGLE:  out->has_shade = out->has_texture = out->has_depth = true; out->shade = decode_shade_setup(&cmd->words[8]); out->texture = decode_texture_setup(&cmd->words[24]); out->depth = decode_depth_setup(&cmd->words[40]); return SR_OK;
    default:                                      return SR_ERROR_BAD_COMMAND;
    }
}

typedef struct raster_rect {
    uint32_t x0;
    uint32_t y0;
    uint32_t x1;
    uint32_t y1;
} raster_rect;

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
    if (!cmd) {
        return SR_ERROR_INVALID_ARGUMENT;
    }
    const raster_decoded_triangle decoded = cmd->decoded.triangle;
    sr_result result = SR_OK;

    if (cmd->id != RDP_CMD_FILL_TRIANGLE && !decoded.has_shade && !decoded.has_texture) {
        return SR_OK;
    }

    const int yh = fixed_ceil_div(decoded.position.yh - 2, 4);
    const int yl = fixed_ceil_div(decoded.position.yl - 2, 4);
    const bool fill_triangle = cmd->id == RDP_CMD_FILL_TRIANGLE;

    if (yl <= yh) {
        return SR_OK;
    }

#if SOFTRDP_ENABLE_PERF_LOG
    LARGE_INTEGER start, end;
    QueryPerformanceCounter(&start);
#endif

    rdp_tile_bounds bounds = {0};
    if (decoded.has_texture) {
        pipeline_resolve_tile_bounds(state, tmem, decoded.position.tile & 7u, &bounds);
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
            result = pipeline_process_triangle_pixel(memory, tmem, state, &decoded, x, y, fill_triangle, &bounds);
            if (result != SR_OK) {
#if SOFTRDP_ENABLE_PERF_LOG
                QueryPerformanceCounter(&end);
                state->triangle_ticks += (end.QuadPart - start.QuadPart);
                state->triangle_count++;
#endif
                return result;
            }
        }
    }

#if SOFTRDP_ENABLE_PERF_LOG
    QueryPerformanceCounter(&end);
    state->triangle_ticks += (end.QuadPart - start.QuadPart);
    state->triangle_count++;
#endif

    return SR_OK;
}

static sr_result submit_texture_rectangle(sr_memory *memory, tmem_state *tmem, rdp_state *state, const rdp_command *cmd)
{
    const rdp_rect_cmd *rect_cmd = &cmd->decoded.rect;
    const uint32_t tile_index = rect_cmd->tile_index;
    const int32_t s0 = rect_cmd->s0;
    const int32_t t0 = rect_cmd->t0;
    const int32_t dsdx = rect_cmd->dsdx;
    const int32_t dtdy = rect_cmd->dtdy;
    const bool flip = rect_cmd->flip;
    
    raster_rect rect = {
        .x0 = rect_cmd->x0,
        .y0 = rect_cmd->y0,
        .x1 = rect_cmd->x1,
        .y1 = rect_cmd->y1
    };
    const uint32_t base_x = rect.x0;
    const uint32_t base_y = rect.y0;

    if (!clip_rect_to_scissor(&rect, state)) {
        return SR_OK;
    }

    rdp_tile_bounds bounds = {0};
    pipeline_resolve_tile_bounds(state, tmem, tile_index, &bounds);

    for (uint32_t y = rect.y0; y <= rect.y1; y++) {
        for (uint32_t x = rect.x0; x <= rect.x1; x++) {
            const int32_t dx = (int32_t)(x - base_x);
            const int32_t dy = (int32_t)(y - base_y);
            const int32_t s_fixed = s0 + (flip ? dy * dtdy : dx * dsdx);
            const int32_t t_fixed = t0 + (flip ? dx * dsdx : dy * dtdy);
            const uint32_t s = s_fixed < 0 ? 0u : (uint32_t)s_fixed >> 5;
            const uint32_t t = t_fixed < 0 ? 0u : (uint32_t)t_fixed >> 5;

            sr_result result = pipeline_process_rect_pixel(memory, tmem, state, tile_index, s, t, x, y, &bounds);
            if (result != SR_OK) {
                return result;
            }
        }
    }

    return SR_OK;
}

static sr_result raster_submit_rectangle_internal(sr_memory *memory, tmem_state *tmem, rdp_state *state, const rdp_command *cmd)
{
    raster_rect rect;

    if (cmd->id == RDP_CMD_TEXTURE_RECTANGLE || cmd->id == RDP_CMD_TEXTURE_RECTANGLE_FLIP) {
        return submit_texture_rectangle(memory, tmem, state, cmd);
    }

    if (cmd->id != RDP_CMD_FILL_RECTANGLE) {
        return SR_OK;
    }

    rect.x0 = cmd->decoded.rect.x0;
    rect.y0 = cmd->decoded.rect.y0;
    rect.x1 = cmd->decoded.rect.x1;
    rect.y1 = cmd->decoded.rect.y1;

    if (state->other_modes.cycle_type == RDP_CYCLE_FILL ||
        state->other_modes.cycle_type == RDP_CYCLE_COPY) {
        rect.y1 = ((cmd->words[0] & 0xfffu) | 3u) >> 2;
    }

    if (!clip_rect_to_scissor(&rect, state)) {
        return SR_OK;
    }

    return framebuffer_fill_rect(memory, state, rect.x0, rect.y0, rect.x1, rect.y1);
}

sr_result raster_submit_rectangle(sr_memory *memory, tmem_state *tmem, rdp_state *state, const rdp_command *cmd)
{
#if SOFTRDP_ENABLE_PERF_LOG
    LARGE_INTEGER start, end;
    QueryPerformanceCounter(&start);
#endif

    sr_result result = raster_submit_rectangle_internal(memory, tmem, state, cmd);

#if SOFTRDP_ENABLE_PERF_LOG
    QueryPerformanceCounter(&end);
    state->rect_ticks += (end.QuadPart - start.QuadPart);
    state->rect_count++;
#endif

    return result;
}
