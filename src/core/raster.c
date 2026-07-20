/*
 * Copyright (c) 2026 PixelMechanic0
 * Licensed under the MIT License.
 *
 * Project: https://github.com/PixelMechanic0/softrdp
 */

#include "raster.h"

#include "framebuffer.h"
#include "pipeline.h"

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
    setup.tile = (uint8_t)((w[0] >> 16) & 7u);
    setup.max_level = (uint8_t)((w[0] >> 19) & 7u);
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


typedef struct raster_span {
    int y;
    int x0;
    int x1;
    raster_coverage_span coverage;
} raster_span;

typedef struct raster_edge_cursor {
    int64_t major[4];
    int64_t upper[4];
    int64_t lower[4];
} raster_edge_cursor;

static void raster_edge_cursor_init(raster_edge_cursor *cursor,
                                    const raster_triangle_setup *setup,
                                    int y)
{
    const int64_t yh_base = (int64_t)setup->yh & ~3ll;
    for (int row = 0; row < 4; row++) {
        const int64_t y_sub = (int64_t)y * 4 + row;
        const int64_t dy_h = y_sub - yh_base;
        cursor->major[row] = (int64_t)setup->xh +
            fixed_floor_div((int64_t)setup->dxhdy * dy_h, 4);
        cursor->upper[row] = (int64_t)setup->xm +
            fixed_floor_div((int64_t)setup->dxmdy * dy_h, 4);
        cursor->lower[row] = (int64_t)setup->xl +
            fixed_floor_div((int64_t)setup->dxldy * (y_sub - setup->ym), 4);
    }
}

static inline void raster_edge_cursor_advance(raster_edge_cursor *cursor,
                                              const raster_triangle_setup *setup)
{
    for (int row = 0; row < 4; row++) {
        cursor->major[row] += setup->dxhdy;
        cursor->upper[row] += setup->dxmdy;
        cursor->lower[row] += setup->dxldy;
    }
}

static int32_t clamp_edge_x(int64_t x, int32_t minimum, int32_t maximum)
{
    if (x < minimum) return minimum;
    if (x > maximum) return maximum;
    return (int32_t)x;
}

static bool triangle_span_for_y(const raster_triangle_setup *setup,
                                const rdp_state *state,
                                const raster_edge_cursor *cursor,
                                int y,
                                raster_span *span)
{
    static const int sample_min[4] = { 0, 2, 0, 2 };
    static const int sample_max[4] = { 4, 6, 4, 6 };
    const int scissor_y0 = state->scissor_y0 ? (int)state->scissor_y0 : 0;
    const int scissor_y1 = state->scissor_y1 ? (int)state->scissor_y1 : 0x1000;
    const int32_t scissor_x0 = state->scissor_x0 ? (int32_t)state->scissor_x0 << 14 : 0;
    const int32_t scissor_x1 = state->scissor_x1 ? (int32_t)state->scissor_x1 << 14 : 0x04000000;
    int min_edge = 0x3fffffff;
    int max_edge = -0x3fffffff;
    int full_x0 = -0x3fffffff;
    int full_x1 = 0x3fffffff;
    uint8_t valid_rows = 0u;

    *span = (raster_span){ .y = y };
    for (int row = 0; row < 4; row++) {
        const int64_t y_sub = (int64_t)y * 4 + row;
        if (y_sub < setup->yh || y_sub >= setup->yl ||
            y_sub < scissor_y0 || y_sub >= scissor_y1) {
            continue;
        }

        const int64_t xh = cursor->major[row];
        const int64_t xl = y_sub < setup->ym
            ? cursor->upper[row] : cursor->lower[row];

        const int32_t left = clamp_edge_x(setup->flip ? xh : xl, scissor_x0, scissor_x1);
        const int32_t right = clamp_edge_x(setup->flip ? xl : xh, scissor_x0, scissor_x1);
        if (left >= right) continue;

        span->coverage.left[row] = left;
        span->coverage.right[row] = right;
        valid_rows |= (uint8_t)(1u << row);
        if (left < min_edge) min_edge = left;
        if (right > max_edge) max_edge = right;

        const int row_full_x0 = fixed_ceil_div(
            (int64_t)left - (int64_t)sample_min[row] * 8192, 65536);
        const int row_full_x1 = fixed_floor_div(
            (int64_t)right - 1 - (int64_t)sample_max[row] * 8192, 65536);
        if (row_full_x0 > full_x0) full_x0 = row_full_x0;
        if (row_full_x1 < full_x1) full_x1 = row_full_x1;
    }

    if (!valid_rows) return false;
    span->coverage.valid_rows = valid_rows;
    span->x0 = fixed_floor_div(min_edge, 65536);
    span->x1 = fixed_floor_div(max_edge, 65536);
    if (valid_rows == 0x0fu) {
        span->coverage.full_x0 = full_x0;
        span->coverage.full_x1 = full_x1;
    } else {
        span->coverage.full_x0 = 1;
        span->coverage.full_x1 = 0;
    }
    return span->x0 <= span->x1;
}

static bool fill_triangle_span_for_y(const raster_triangle_setup *setup,
                                     const rdp_state *state,
                                     int y,
                                     raster_span *span)
{
    const int64_t y_center = (int64_t)y * 4 + 2;
    const int64_t yh_base = (int64_t)setup->yh & ~3ll;
    const int64_t dy_h = y_center - yh_base;
    const int64_t xh = (int64_t)setup->xh +
        fixed_floor_div((int64_t)setup->dxhdy * dy_h, 4);
    const int64_t xl = y_center < setup->ym
        ? (int64_t)setup->xm + fixed_floor_div((int64_t)setup->dxmdy * dy_h, 4)
        : (int64_t)setup->xl + fixed_floor_div(
            (int64_t)setup->dxldy * (y_center - setup->ym), 4);
    *span = (raster_span){ .y = y };
    if (setup->flip) {
        span->x0 = fixed_ceil_div(xh, 0x10000);
        span->x1 = fixed_ceil_div(xl, 0x10000) - 1;
    } else {
        span->x0 = fixed_ceil_div(xl, 0x10000);
        span->x1 = fixed_ceil_div(xh, 0x10000) - 1;
    }
    const int min_x = state->scissor_x0 ? (int)((state->scissor_x0 + 3u) >> 2) : 0;
    const int max_x = state->scissor_x1 ? (int)((state->scissor_x1 - 1u) >> 2) : 0x3ff;
    const int min_y = state->scissor_y0 ? (int)((state->scissor_y0 + 3u) >> 2) : 0;
    const int max_y = state->scissor_y1 ? (int)((state->scissor_y1 - 1u) >> 2) : 0x3ff;
    if (y < min_y || y > max_y) return false;
    if (span->x0 < min_x) span->x0 = min_x;
    if (span->x1 > max_x) span->x1 = max_x;
    span->coverage.full_x0 = span->x0;
    span->coverage.full_x1 = span->x1;
    span->coverage.valid_rows = 0x0fu;
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

sr_result raster_submit_triangle(sr_memory *memory,
                                 tmem_state *tmem,
                                 const rdp_state *state,
                                 const rdp_command *cmd)
{
    if (!cmd) {
        return SR_ERROR_INVALID_ARGUMENT;
    }
    const raster_decoded_triangle decoded = cmd->decoded.triangle;
    sr_result result = SR_OK;

    const bool fill_triangle = cmd->id == RDP_CMD_FILL_TRIANGLE;
    const int yh = fill_triangle
        ? fixed_ceil_div(decoded.position.yh - 2, 4)
        : fixed_floor_div(decoded.position.yh, 4);
    const int yl = fill_triangle
        ? fixed_ceil_div(decoded.position.yl - 2, 4)
        : fixed_ceil_div(decoded.position.yl, 4);

    if (yl <= yh) {
        return SR_OK;
    }

    rdp_primitive_state primitive;
    pipeline_compile_triangle(&primitive,
                              state,
                              tmem,
                              &decoded,
                              fill_triangle);

    raster_edge_cursor edge_cursor;
    if (!fill_triangle)
        raster_edge_cursor_init(&edge_cursor, &decoded.position, yh);

    for (int y = yh; y < yl; y++) {
        raster_span span;
        const bool has_span = fill_triangle
            ? fill_triangle_span_for_y(&decoded.position, state, y, &span)
            : triangle_span_for_y(&decoded.position, state, &edge_cursor, y, &span);
        if (!fill_triangle)
            raster_edge_cursor_advance(&edge_cursor, &decoded.position);
        if (!has_span) {
            continue;
        }

        rdp_span_work work;
        pipeline_setup_triangle_span(&primitive, span.x0, span.x1, y, &work);
        work.coverage = span.coverage;
        result = pipeline_render_span(memory, &primitive, &work);
        if (result != SR_OK) {
            return result;
        }
    }

    return SR_OK;
}

static sr_result submit_texture_rectangle(sr_memory *memory,
                                          tmem_state *tmem,
                                          const rdp_state *state,
                                          const rdp_command *cmd)
{
    const rdp_rect_cmd *rect_cmd = &cmd->decoded.rect;
    const uint32_t tile_index = rect_cmd->tile_index;
    const int32_t s0 = rect_cmd->s0 * 32;
    const int32_t t0 = rect_cmd->t0 * 32;
    /* Copy mode advances one texture coordinate step per four-pixel copy
     * group. This scalar renderer emits one framebuffer pixel at a time, so
     * convert the command's group derivative to a per-pixel derivative. */
    const int32_t dsdx = state->other_modes.cycle_type == RDP_CYCLE_COPY
        ? rect_cmd->dsdx >> 2 : rect_cmd->dsdx;
    const int32_t dtdy = rect_cmd->dtdy;
    const bool flip = rect_cmd->flip;
    
    raster_rect rect = {
        .x0 = rect_cmd->x0,
        .y0 = rect_cmd->y0,
        .x1 = rect_cmd->x1,
        .y1 = rect_cmd->y1
    };
    if (state->other_modes.cycle_type == RDP_CYCLE_1 ||
        state->other_modes.cycle_type == RDP_CYCLE_2) {
        if (rect_cmd->xl == 0u || rect_cmd->yl == 0u) return SR_OK;
        rect.x1 = (rect_cmd->xl - 1u) >> 2;
        rect.y1 = (rect_cmd->yl - 1u) >> 2;
    }
    const uint32_t base_x = rect.x0;
    const uint32_t base_y = rect.y0;

    if (!clip_rect_to_scissor(&rect, state)) {
        return SR_OK;
    }

    rdp_primitive_state primitive;
    pipeline_compile_rectangle(&primitive, state, tmem, tile_index);

    for (uint32_t y = rect.y0; y <= rect.y1; y++) {
        const int32_t dx = (int32_t)(rect.x0 - base_x);
        const int32_t dy = (int32_t)(y - base_y);
        int32_t s_fixed = s0 + (flip ? dy * dtdy : dx * dsdx);
        int32_t t_fixed = t0 + (flip ? dx * dsdx : dy * dtdy);
        const int32_t span_dsdx = flip ? 0 : dsdx;
        const int32_t span_dtdx = flip ? dsdx : 0;
        if (state->other_modes.cycle_type == RDP_CYCLE_2) {
            /* The two-cycle span pipeline consumes rectangle texture
             * coordinates two horizontal steps ahead of the write position. */
            s_fixed -= 2 * span_dsdx;
            t_fixed -= 2 * span_dtdx;
        }
        rdp_span_work work;

        pipeline_setup_rectangle_span((int)rect.x0,
                                      (int)rect.x1,
                                      (int)y,
                                      s_fixed,
                                      t_fixed,
                                      span_dsdx,
                                      span_dtdx,
                                      &work);
        work.texture_coord_shift = 5u;
        sr_result result = pipeline_render_span(memory, &primitive, &work);
        if (result != SR_OK) {
            return result;
        }
    }

    return SR_OK;
}

static sr_result raster_submit_rectangle_internal(sr_memory *memory,
                                                  tmem_state *tmem,
                                                  const rdp_state *state,
                                                  const rdp_command *cmd)
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

    if (state->other_modes.cycle_type != RDP_CYCLE_FILL &&
        state->other_modes.cycle_type != RDP_CYCLE_COPY) {
        rdp_primitive_state primitive;
        pipeline_compile_color_rectangle(&primitive, state, tmem);
        for (uint32_t y = rect.y0; y <= rect.y1; y++) {
            rdp_span_work work;
            pipeline_setup_rectangle_span((int)rect.x0, (int)rect.x1, (int)y,
                                          0, 0, 0, 0, &work);
            const sr_result result = pipeline_render_span(memory, &primitive, &work);
            if (result != SR_OK) return result;
        }
        return SR_OK;
    }

    rdp_framebuffer_state framebuffer;
    pipeline_compile_framebuffer(&framebuffer, state);
    return framebuffer_fill_rect(memory,
                                 &framebuffer,
                                 rect.x0,
                                 rect.y0,
                                 rect.x1,
                                 rect.y1);
}

sr_result raster_submit_rectangle(sr_memory *memory,
                                  tmem_state *tmem,
                                  const rdp_state *state,
                                  const rdp_command *cmd)
{
    return raster_submit_rectangle_internal(memory, tmem, state, cmd);
}
