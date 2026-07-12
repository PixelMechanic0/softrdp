#include "vi.h"

#include "pipeline.h"
#include "rdp_memory.h"

#include <string.h>

#define VI_TYPE_MASK 3u
#define VI_TYPE_RGBA5551 2u
#define VI_TYPE_RGBA8888 3u
#define VI_CONTROL_GAMMA_DITHER (1u << 2)
#define VI_CONTROL_GAMMA (1u << 3)
#define VI_CONTROL_DIVOT (1u << 4)
#define VI_CONTROL_SERRATE (1u << 6)
#define VI_CONTROL_AA_MODE_SHIFT 8u
#define VI_CONTROL_DITHER_FILTER (1u << 16)
#define VI_AA_REPLICATE 3u
#define VI_V_SYNC_NTSC 525u
#define VI_H_OFFSET_NTSC 108
#define VI_H_OFFSET_PAL 128
#define VI_V_OFFSET_NTSC 34
#define VI_V_OFFSET_PAL 44

typedef struct vi_row_cache {
    sr_rgba8 rows[2][VI_MAX_SOURCE_WIDTH];
    uint32_t row_index[2];
    bool valid[2];
    uint32_t next_slot;
} vi_row_cache;

void vi_init(vi_state *vi)
{
    if (vi) memset(vi, 0, sizeof(*vi));
}

void vi_latch_registers(vi_state *vi, const sr_host_interface *host)
{
    if (!vi || !host) return;
#define LATCH(reg) (host->vi_regs[reg] ? *host->vi_regs[reg] : 0u)
    vi->control = LATCH(SR_VI_STATUS);
    vi->origin = LATCH(SR_VI_ORIGIN);
    vi->width = LATCH(SR_VI_WIDTH);
    vi->current = LATCH(SR_VI_CURRENT);
    vi->v_sync = LATCH(SR_VI_V_SYNC);
    vi->h_start = LATCH(SR_VI_H_START);
    vi->v_start = LATCH(SR_VI_V_START);
    vi->x_scale = LATCH(SR_VI_X_SCALE);
    vi->y_scale = LATCH(SR_VI_Y_SCALE);
#undef LATCH
}

void vi_build_scanout_plan(const vi_state *vi, const sr_memory *memory,
                           vi_scanout_plan *plan)
{
    memset(plan, 0, sizeof(*plan));
    if (!vi || !memory || !memory->rdram) return;

    const uint32_t type = vi->control & VI_TYPE_MASK;
    const uint32_t origin = vi->origin & 0x00ffffffu;
    const uint32_t source_stride = vi->width & 0xfffu;
    const uint32_t x_add = vi->x_scale & 0xfffu;
    const uint32_t y_add = vi->y_scale & 0xfffu;
    uint32_t x_start = (vi->x_scale >> 16) & 0xfffu;
    uint32_t y_start = (vi->y_scale >> 16) & 0xfffu;
    int32_t h_start = (int32_t)((vi->h_start >> 16) & 0x3ffu);
    int32_t h_end = (int32_t)(vi->h_start & 0x3ffu);
    int32_t v_start = (int32_t)((vi->v_start >> 16) & 0x3ffu);
    int32_t v_end = (int32_t)(vi->v_start & 0x3ffu);
    const bool pal = (vi->v_sync & 0x3ffu) > VI_V_SYNC_NTSC + 25u;
    const int32_t h_offset = pal ? VI_H_OFFSET_PAL : VI_H_OFFSET_NTSC;
    const int32_t v_offset = pal ? VI_V_OFFSET_PAL : VI_V_OFFSET_NTSC;

    if ((type != VI_TYPE_RGBA5551 && type != VI_TYPE_RGBA8888) ||
        origin == 0 || source_stride == 0 ||
        source_stride > VI_MAX_SOURCE_WIDTH || x_add == 0 || y_add == 0 ||
        h_end <= h_start || v_end <= v_start) {
        return;
    }

    h_start -= h_offset;
    h_end -= h_offset;
    if (h_start < 0) {
        x_start += x_add * (uint32_t)(-h_start);
        h_start = 0;
    }
    if (h_end > (int32_t)VI_MAX_OUTPUT_WIDTH) h_end = VI_MAX_OUTPUT_WIDTH;

    int32_t output_width = h_end - h_start;
    int32_t output_height = (v_end - v_start) >> 1;
    v_start = (v_start - v_offset) / 2;
    if (v_start < 0) {
        y_start += y_add * (uint32_t)(-v_start);
        v_start = 0;
    }
    if (output_height > (int32_t)VI_MAX_OUTPUT_HEIGHT - v_start)
        output_height = (int32_t)VI_MAX_OUTPUT_HEIGHT - v_start;
    if (output_width <= 0 || output_height <= 0) return;

    plan->origin = origin;
    plan->source_stride = source_stride;
    plan->bytes_per_pixel = type == VI_TYPE_RGBA5551 ? 2u : 4u;
    plan->output_width = (uint32_t)output_width;
    plan->output_height = (uint32_t)output_height;
    plan->display_width = plan->output_width;
    plan->display_height = (uint32_t)(((uint64_t)plan->output_height * 2u *
                           VI_V_SYNC_NTSC) /
                           ((vi->v_sync & 0x3ffu) ?
                            (vi->v_sync & 0x3ffu) : VI_V_SYNC_NTSC));
    if (!plan->display_height) plan->display_height = plan->output_height;
    plan->aa_mode = (vi->control >> VI_CONTROL_AA_MODE_SHIFT) & 3u;
    plan->serrate = (vi->control & VI_CONTROL_SERRATE) != 0;
    plan->gamma_enable = (vi->control & VI_CONTROL_GAMMA) != 0;
    plan->gamma_dither_enable = (vi->control & VI_CONTROL_GAMMA_DITHER) != 0;
    plan->divot_enable = (vi->control & VI_CONTROL_DIVOT) != 0;
    plan->dither_filter_enable = (vi->control & VI_CONTROL_DITHER_FILTER) != 0;

    uint32_t max_x = 0;
    uint32_t max_y = 0;
    for (uint32_t x = 0; x < plan->output_width; x++) {
        const uint32_t coordinate = x_start + x * x_add;
        plan->x_samples[x].source_x = (uint16_t)(coordinate >> 10);
        plan->x_samples[x].fraction = (uint8_t)((coordinate >> 5) & 31u);
        if (plan->x_samples[x].source_x > max_x) max_x = plan->x_samples[x].source_x;
    }
    for (uint32_t y = 0; y < plan->output_height; y++) {
        const uint32_t coordinate = y_start + y * y_add;
        plan->y_samples[y].source_y = (uint16_t)(coordinate >> 10);
        plan->y_samples[y].fraction = (uint8_t)((coordinate >> 5) & 31u);
        if (plan->y_samples[y].source_y > max_y) max_y = plan->y_samples[y].source_y;
    }

    if (max_x >= source_stride) return;

    const bool interpolate = plan->aa_mode != VI_AA_REPLICATE;
    if (interpolate) max_y++;

    const uint64_t last_pixel = (uint64_t)max_y * source_stride + max_x;
    const uint64_t last_byte = (uint64_t)origin +
                               last_pixel * plan->bytes_per_pixel +
                               plan->bytes_per_pixel - 1u;
    if (last_byte >= memory->rdram_size) {
        plan->state = VI_SCANOUT_INVALID_MEMORY;
        return;
    }
    plan->state = VI_SCANOUT_READY;
}

static sr_result decode_row(const vi_scanout_plan *plan,
                            const sr_memory *memory, uint32_t source_y,
                            sr_rgba8 *row)
{
    const uint64_t row_base = (uint64_t)plan->origin +
                              (uint64_t)source_y * plan->source_stride *
                              plan->bytes_per_pixel;
    for (uint32_t x = 0; x < plan->source_stride; x++) {
        const uint32_t address = (uint32_t)(row_base +
                                 (uint64_t)x * plan->bytes_per_pixel);
        if (plan->bytes_per_pixel == 2u) {
            uint16_t raw;
            if (!sr_memory_read_be16(memory, address, &raw))
                return SR_ERROR_INVALID_ARGUMENT;
            const rdp_color color = pipeline_rgba5551_to_color(raw);
            row[x] = (sr_rgba8){color.r, color.g, color.b, color.a};
        } else {
            uint32_t raw;
            if (!sr_memory_read_be32(memory, address, &raw))
                return SR_ERROR_INVALID_ARGUMENT;
            row[x] = (sr_rgba8){(uint8_t)(raw >> 24), (uint8_t)(raw >> 16),
                                (uint8_t)(raw >> 8), (uint8_t)raw};
        }
    }
    return SR_OK;
}

static sr_result get_cached_row(vi_row_cache *cache,
                                const vi_scanout_plan *plan,
                                const sr_memory *memory, uint32_t source_y,
                                const sr_rgba8 **row)
{
    for (uint32_t slot = 0; slot < 2; slot++) {
        if (cache->valid[slot] && cache->row_index[slot] == source_y) {
            *row = cache->rows[slot];
            return SR_OK;
        }
    }
    const uint32_t slot = cache->next_slot++ & 1u;
    const sr_result result = decode_row(plan, memory, source_y,
                                        cache->rows[slot]);
    if (result != SR_OK) return result;
    cache->valid[slot] = true;
    cache->row_index[slot] = source_y;
    *row = cache->rows[slot];
    return SR_OK;
}

static sr_rgba8 lerp_color(sr_rgba8 a, sr_rgba8 b, uint32_t fraction)
{
#define LERP(c) ((uint8_t)((uint32_t)a.c + \
    (((int32_t)b.c - (int32_t)a.c) * (int32_t)fraction + 16) / 32))
    const sr_rgba8 result = {LERP(r), LERP(g), LERP(b), LERP(a)};
#undef LERP
    return result;
}

sr_result vi_execute_scanout(const vi_scanout_plan *plan,
                             const sr_memory *memory,
                             sr_framebuffer *out)
{
    if (!plan || !memory || !out) return SR_ERROR_INVALID_ARGUMENT;
    out->valid = false;
    if (plan->state != VI_SCANOUT_READY) {
        out->width = plan->output_width;
        out->height = plan->output_height;
        return SR_OK;
    }
    const uint32_t width = out->width ? out->width : plan->output_width;
    const uint32_t height = out->height ? out->height : plan->output_height;
    const uint32_t stride = out->stride_pixels ? out->stride_pixels : width;
    if (!out->pixels || width != plan->output_width ||
        height != plan->output_height || stride < width)
        return SR_ERROR_INVALID_ARGUMENT;

    vi_row_cache cache;
    memset(&cache, 0, sizeof(cache));
    const bool interpolate = plan->aa_mode != VI_AA_REPLICATE;

    for (uint32_t y = 0; y < height; y++) {
        const vi_y_sample ys = plan->y_samples[y];
        const sr_rgba8 *row0;
        const sr_rgba8 *row1 = NULL;
        sr_result result = get_cached_row(&cache, plan, memory,
                                          ys.source_y, &row0);
        if (result != SR_OK) return result;
        if (interpolate && ys.fraction) {
            result = get_cached_row(&cache, plan, memory,
                                    ys.source_y + 1u, &row1);
            if (result != SR_OK) return result;
        }

        sr_rgba8 *destination = out->pixels + y * stride;
        for (uint32_t x = 0; x < width; x++) {
            const vi_x_sample xs = plan->x_samples[x];
            sr_rgba8 color = row0[xs.source_x];
            if (interpolate && xs.fraction) {
                const uint32_t next_x = xs.source_x + 1u < plan->source_stride ?
                                        xs.source_x + 1u : xs.source_x;
                color = lerp_color(color, row0[next_x],
                                   xs.fraction);
            }
            if (row1) {
                sr_rgba8 lower = row1[xs.source_x];
                if (interpolate && xs.fraction) {
                    const uint32_t next_x = xs.source_x + 1u < plan->source_stride ?
                                            xs.source_x + 1u : xs.source_x;
                    lower = lerp_color(lower, row1[next_x],
                                       xs.fraction);
                }
                color = lerp_color(color, lower, ys.fraction);
            }
            destination[x] = color;
        }
    }
    out->width = width;
    out->height = height;
    out->stride_pixels = stride;
    out->valid = true;
    return SR_OK;
}
