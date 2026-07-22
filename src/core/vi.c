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
    const uint32_t origin = (vi->origin & 0x00ffffffu) & (memory->rdram_size - 1u);
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

    /* A non-RGBA pixel type is a genuine blank signal: leave state BLANK so the
     * presenter shows black. */
    if (type != VI_TYPE_RGBA5551 && type != VI_TYPE_RGBA8888) {
        return;
    }
    /* Valid pixel type but the frame is not renderable this refresh (framebuffer
     * or timing not fully programmed, e.g. H_START still zero). Hold the last
     * frame rather than flashing black. source_stride is the row pitch, not the
     * sampled width, so a wide stride is fine; the sampled extent is bounded
     * later (max_x < VI_MAX_SOURCE_WIDTH) and validated by the memory check. */
    if (origin == 0 || source_stride == 0 ||
        x_add == 0 || y_add == 0 ||
        h_end <= h_start || v_end <= v_start) {
        plan->state = VI_SCANOUT_HOLD;
        return;
    }

    h_start -= h_offset;
    h_end -= h_offset;
    bool left_clamp = false;
    bool right_clamp = false;
    if (h_start < 0) {
        x_start += x_add * (uint32_t)(-h_start);
        h_start = 0;
        left_clamp = true;
    }
    if (h_end > (int32_t)VI_MAX_OUTPUT_WIDTH) {
        h_end = VI_MAX_OUTPUT_WIDTH;
        right_clamp = true;
    }

    int32_t output_width = h_end - h_start;
    /* Interlaced (serrate) frames hold the full image in the framebuffer while
     * the VI scans out every other line per field. Read every source line into a
     * full-height progressive frame instead: this shows the full vertical
     * resolution the game rendered and removes the one-line inter-field jitter.
     * The field's half-line origin offset is dropped so each output line maps
     * cleanly onto one source line. */
    const bool serrate = (vi->control & VI_CONTROL_SERRATE) != 0;
    const uint32_t effective_y_add = serrate ? (y_add >> 1) : y_add;
    /* The one-line offset that interleaves the two interlaced fields alternates
     * in the low bit of V_START every frame. For a stable deinterlaced image we
     * drop it (and the field's half-line source origin), so both fields produce
     * identical geometry and the picture no longer jumps by a line each frame. */
    if (serrate) {
        v_start &= ~1;
        /* The two interlaced fields are offset by half a field step. Nudge one
         * field's sampling origin by half the source step (y_add/2) so both
         * fields land on the same output rows and the progressive image stops
         * jumping. This scales with y_add rather than a fixed constant: for a
         * full-frame buffer (y_add ~2.0) the fields are a whole source line apart,
         * for a per-field buffer (y_add ~1.0) half a line. Parity from V_CURRENT;
         * flip vi_field_parity_invert if a title uses the opposite convention. */
        const bool vi_field_parity_invert = false;
        const uint32_t parity = (vi->current & 1u) ^ (uint32_t)vi_field_parity_invert;
        y_start = parity ? 0u : (y_add >> 1);
    }
    const int32_t field_height = (v_end - v_start) >> 1;
    int32_t output_height = serrate ? (field_height << 1) : field_height;
    v_start = (v_start - v_offset) / 2;
    if (v_start < 0) {
        y_start += effective_y_add * (uint32_t)(-v_start);
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
    /* The VI blanks an 8-pixel left and 7-pixel right guard band unless that
     * edge is already clamped to the screen border. This is where the filter
     * window would sample past the framebuffer, so it also keeps the fetch from
     * reading beyond WIDTH into the next scanline. */
    const uint32_t left_border = left_clamp ? 0u : 8u;
    const uint32_t right_border = right_clamp ? 0u : 7u;
    plan->active_x_begin = left_border < plan->output_width
        ? left_border : plan->output_width;
    plan->active_x_end = plan->output_width > right_border
        ? plan->output_width - right_border : 0u;
    if (plan->active_x_end < plan->active_x_begin)
        plan->active_x_end = plan->active_x_begin;
    plan->display_width = plan->output_width;
    /* Progressive serrate output already contains both fields, so it must not be
     * line-doubled for display the way a single 240p field is. */
    plan->display_height = (uint32_t)(((uint64_t)plan->output_height *
                           (serrate ? 1u : 2u) * VI_V_SYNC_NTSC) /
                           ((vi->v_sync & 0x3ffu) ?
                            (vi->v_sync & 0x3ffu) : VI_V_SYNC_NTSC));
    if (!plan->display_height) plan->display_height = plan->output_height;
    plan->aa_mode = (vi->control >> VI_CONTROL_AA_MODE_SHIFT) & 3u;
    plan->serrate = serrate;
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
        /* Only active columns are fetched, so blanked guard-band columns must
         * not widen the source extent or the memory range check. */
        if (x >= plan->active_x_begin && x < plan->active_x_end &&
            plan->x_samples[x].source_x > max_x)
            max_x = plan->x_samples[x].source_x;
    }
    for (uint32_t y = 0; y < plan->output_height; y++) {
        const uint32_t coordinate = y_start + y * effective_y_add;
        plan->y_samples[y].source_y = (uint16_t)(coordinate >> 10);
        plan->y_samples[y].fraction = (uint8_t)((coordinate >> 5) & 31u);
        if (plan->y_samples[y].source_y > max_y) max_y = plan->y_samples[y].source_y;
    }

    if (max_x >= VI_MAX_SOURCE_WIDTH) {
        plan->state = VI_SCANOUT_HOLD;
        return;
    }
    plan->source_width = max_x + 1u;

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

/* One-time decode table for RGBA5551 texels (indexed by big-endian halfword). */
static sr_rgba8 vi_rgba5551_table[65536];
static bool vi_rgba5551_table_ready;

static void vi_build_rgba5551_table(void)
{
    for (uint32_t v = 0; v < 65536u; v++) {
        const rdp_color color = pipeline_rgba5551_to_color((uint16_t)v);
        vi_rgba5551_table[v] = (sr_rgba8){color.r, color.g, color.b, color.a};
    }
    vi_rgba5551_table_ready = true;
}

/* Plan is VI_SCANOUT_READY, so every address touched here was proven in range
 * during vi_build_scanout_plan(); use the unchecked fast accessors. */
static void decode_row(const vi_scanout_plan *plan,
                       const sr_memory *memory, uint32_t source_y,
                       sr_rgba8 *restrict row)
{
    const uint32_t row_base = plan->origin +
                              source_y * plan->source_stride *
                              plan->bytes_per_pixel;
    const uint32_t width = plan->source_width;
    if (plan->bytes_per_pixel == 2u) {
        const sr_rgba8 *restrict table = vi_rgba5551_table;
        for (uint32_t x = 0; x < width; x++) {
            const uint16_t raw = sr_memory_read_be16_fast(memory,
                                                          row_base + x * 2u);
            row[x] = table[raw];
        }
    } else {
        for (uint32_t x = 0; x < width; x++) {
            const uint32_t raw = sr_memory_read_be32_fast(memory,
                                                         row_base + x * 4u);
            row[x] = (sr_rgba8){(uint8_t)(raw >> 24), (uint8_t)(raw >> 16),
                                (uint8_t)(raw >> 8), (uint8_t)raw};
        }
    }
}

static const sr_rgba8 *get_cached_row(vi_row_cache *cache,
                                      const vi_scanout_plan *plan,
                                      const sr_memory *memory,
                                      uint32_t source_y)
{
    for (uint32_t slot = 0; slot < 2; slot++) {
        if (cache->valid[slot] && cache->row_index[slot] == source_y)
            return cache->rows[slot];
    }
    const uint32_t slot = cache->next_slot++ & 1u;
    decode_row(plan, memory, source_y, cache->rows[slot]);
    cache->valid[slot] = true;
    cache->row_index[slot] = source_y;
    return cache->rows[slot];
}

/* Blend all four channels at once via SWAR: result = (a*(32-f) + b*f + 16) >> 5.
 * With f in [0,31] every 8-bit lane stays <= 255*32 = 8160, so two channels
 * pack safely into the 16-bit lanes of a 32-bit word with no cross-lane carry. */
static inline sr_rgba8 lerp_color(sr_rgba8 a, sr_rgba8 b, uint32_t fraction)
{
    uint32_t av, bv;
    memcpy(&av, &a, 4);
    memcpy(&bv, &b, 4);
    const uint32_t inv = 32u - fraction;
    const uint32_t lo = ((av & 0x00ff00ffu) * inv +
                         (bv & 0x00ff00ffu) * fraction + 0x00100010u) >> 5;
    const uint32_t hi = (((av >> 8) & 0x00ff00ffu) * inv +
                         ((bv >> 8) & 0x00ff00ffu) * fraction + 0x00100010u) >> 5;
    const uint32_t rv = (lo & 0x00ff00ffu) | ((hi & 0x00ff00ffu) << 8);
    sr_rgba8 result;
    memcpy(&result, &rv, 4);
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

    if (plan->bytes_per_pixel == 2u && !vi_rgba5551_table_ready)
        vi_build_rgba5551_table();

    vi_row_cache cache;
    memset(&cache, 0, sizeof(cache));
    const bool interpolate = plan->aa_mode != VI_AA_REPLICATE;
    const uint32_t source_width = plan->source_width;
    const sr_rgba8 border = {0, 0, 0, 0};
    const uint32_t active_begin = plan->active_x_begin < width
        ? plan->active_x_begin : width;
    const uint32_t active_end = plan->active_x_end < width
        ? plan->active_x_end : width;

    for (uint32_t y = 0; y < height; y++) {
        const vi_y_sample ys = plan->y_samples[y];
        const sr_rgba8 *restrict row0 = get_cached_row(&cache, plan, memory,
                                                       ys.source_y);
        const sr_rgba8 *restrict row1 = (interpolate && ys.fraction) ?
            get_cached_row(&cache, plan, memory, ys.source_y + 1u) : NULL;
        sr_rgba8 *restrict destination = out->pixels + y * stride;

        for (uint32_t x = 0; x < active_begin; x++) destination[x] = border;
        for (uint32_t x = active_end; x < width; x++) destination[x] = border;

        if (!interpolate) {
            /* Point sampling: straight gather, no per-pixel branching. */
            for (uint32_t x = active_begin; x < active_end; x++)
                destination[x] = row0[plan->x_samples[x].source_x];
            continue;
        }

        const uint32_t y_fraction = ys.fraction;
        for (uint32_t x = active_begin; x < active_end; x++) {
            const vi_x_sample xs = plan->x_samples[x];
            const uint32_t next_x = xs.source_x + 1u < source_width ?
                                    xs.source_x + 1u : xs.source_x;
            sr_rgba8 color = xs.fraction ?
                lerp_color(row0[xs.source_x], row0[next_x], xs.fraction) :
                row0[xs.source_x];
            if (row1) {
                const sr_rgba8 lower = xs.fraction ?
                    lerp_color(row1[xs.source_x], row1[next_x], xs.fraction) :
                    row1[xs.source_x];
                color = lerp_color(color, lower, y_fraction);
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
