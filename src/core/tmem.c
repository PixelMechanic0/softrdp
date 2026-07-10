#include "tmem.h"

#include "rdp_commands.h"
#include "rdp_memory.h"

#include <string.h>

#if SOFTRDP_ENABLE_PERF_LOG
#define NOMINMAX
#include <windows.h>
#endif

void tmem_init(tmem_state *tmem)
{
    memset(tmem, 0, sizeof(*tmem));
}

static bool texture_state_supports_load(const rdp_state *state, const rdp_tile *tile)
{
    if (state->texture_image.format != tile->format ||
        state->texture_image.size != tile->size) {
        return false;
    }

    if (tile->format == RDP_FORMAT_RGBA) {
        return tile->size == RDP_SIZE_16BPP || tile->size == RDP_SIZE_32BPP;
    }

    if (tile->format == RDP_FORMAT_IA) {
        return tile->size == RDP_SIZE_16BPP;
    }

    return false;
}

static void record_loaded_tile(tmem_state *tmem,
                               uint32_t tile_index,
                               uint32_t width,
                               uint32_t height,
                               uint32_t stride,
                               uint32_t sl,
                               uint32_t tl,
                               uint32_t sh,
                               uint32_t th)
{
    tmem->tile_width[tile_index] = (uint16_t)width;
    tmem->tile_height[tile_index] = (uint16_t)height;
    tmem->tile_stride[tile_index] = (uint16_t)stride;
    tmem->tile_sl[tile_index] = (uint16_t)sl;
    tmem->tile_tl[tile_index] = (uint16_t)tl;
    tmem->tile_sh[tile_index] = (uint16_t)sh;
    tmem->tile_th[tile_index] = (uint16_t)th;
    tmem->loads_seen++;
}

static bool tmem_write_be16(tmem_state *tmem, uint32_t addr, uint16_t value)
{
    if (!tmem || addr + 1u >= SR_TMEM_SIZE) {
        return false;
    }

    tmem->bytes[addr + 0u] = (uint8_t)(value >> 8);
    tmem->bytes[addr + 1u] = (uint8_t)value;
    return true;
}

static sr_result load_16bpp_tile(tmem_state *tmem, sr_memory *memory, const rdp_state *state, const rdp_tile *tile, const rdp_command *cmd)
{
    const uint32_t tile_index = cmd->decoded.load.tile_index;
    const uint32_t sl = cmd->decoded.load.sl >> 2;
    const uint32_t tl = cmd->decoded.load.tl >> 2;
    const uint32_t sh = cmd->decoded.load.sh >> 2;
    const uint32_t th = cmd->decoded.load.th >> 2;

    if (sh < sl || th < tl) {
        return SR_ERROR_INVALID_ARGUMENT;
    }

    const uint32_t width = sh - sl + 1u;
    const uint32_t height = th - tl + 1u;
    const uint32_t stride = tile->line ? tile->line : tmem_align_row_stride(width * 2u);

    for (uint32_t y = 0; y < height; y++) {
        for (uint32_t x = 0; x < width; x++) {
            uint16_t texel;
            tmem_texel_address dst;
            const uint32_t src_pixel = (tl + y) * state->texture_image.width + (sl + x);
            const uint32_t src_addr = state->texture_image.address + src_pixel * 2u;

            if (!sr_memory_read_be16(memory, src_addr, &texel)) {
                return SR_ERROR_INVALID_ARGUMENT;
            }

            if (!tmem_resolve_rgba16_address_raw(tile, stride, x, y, &dst) ||
                !tmem_write_be16(tmem, dst.byte, texel)) {
                return SR_ERROR_INVALID_ARGUMENT;
            }
        }
    }

    record_loaded_tile(tmem, tile_index, width, height, stride, sl, tl, sh, th);
    return SR_OK;
}

static sr_result load_16bpp_block(tmem_state *tmem, sr_memory *memory, const rdp_state *state, const rdp_tile *tile, const rdp_command *cmd)
{
    const uint32_t tile_index = cmd->decoded.load.tile_index;
    const uint32_t sl = cmd->decoded.load.sl;
    const uint32_t tl = cmd->decoded.load.tl;
    const uint32_t sh = cmd->decoded.load.sh;
    const uint32_t dxt = cmd->decoded.load.th;

    if (sh < sl) {
        return SR_ERROR_INVALID_ARGUMENT;
    }

    const uint32_t texel_count = ((sh - sl) + 1u) & 0xfffu;
    if (texel_count == 0) {
        return SR_ERROR_INVALID_ARGUMENT;
    }

    const uint32_t start_s = sl >> 2;
    const uint32_t start_t = tl >> 2;
    const uint32_t tmem16_base = tile->tmem >> 1;
    const uint32_t stride64 = tile->line >> 3;

    for (uint32_t x = 0; x < texel_count; x++) {
        uint16_t texel;
        const uint32_t src_group = x >> 2;
        const uint32_t dxt_row = dxt ? ((src_group * dxt) >> 11) : 0u;
        const uint32_t src_pixel = start_t * state->texture_image.width + start_s + x;
        const uint32_t src_addr = state->texture_image.address + src_pixel * 2u;
        const uint32_t dst_word64 = src_group + dxt_row * stride64;
        const uint32_t dst_lane = (x & 3u) ^ ((dxt_row & 1u) << 1);
        const uint32_t dst_tmem16 = (tmem16_base + (dst_word64 << 2) + dst_lane) & 0x7ffu;
        const uint32_t dst_addr = (dst_tmem16 ^ 1u) << 1;

        if (!sr_memory_read_be16(memory, src_addr, &texel)) {
            return SR_ERROR_INVALID_ARGUMENT;
        }

        if (!tmem_write_be16(tmem, dst_addr, texel)) {
            return SR_ERROR_INVALID_ARGUMENT;
        }
    }

    record_loaded_tile(tmem, tile_index, texel_count, 1u, tile->line, 0u, 0u, texel_count - 1u, 0u);
    return SR_OK;
}

static sr_result load_rgba32_tile(tmem_state *tmem, sr_memory *memory, const rdp_state *state, const rdp_tile *tile, const rdp_command *cmd)
{
    const uint32_t tile_index = cmd->decoded.load.tile_index;
    const uint32_t sl = cmd->decoded.load.sl >> 2;
    const uint32_t tl = cmd->decoded.load.tl >> 2;
    const uint32_t sh = cmd->decoded.load.sh >> 2;
    const uint32_t th = cmd->decoded.load.th >> 2;

    if (sh < sl || th < tl) {
        return SR_ERROR_INVALID_ARGUMENT;
    }

    const uint32_t width = sh - sl + 1u;
    const uint32_t height = th - tl + 1u;
    const uint32_t stride = tile->line ? tile->line : tmem_align_row_stride(width * 2u);

    for (uint32_t y = 0; y < height; y++) {
        for (uint32_t x = 0; x < width; x++) {
            uint32_t texel;
            tmem_texel_address dst;
            const uint32_t src_pixel = (tl + y) * state->texture_image.width + (sl + x);
            const uint32_t src_addr = state->texture_image.address + src_pixel * 4u;

            if (!sr_memory_read_be32(memory, src_addr, &texel)) {
                return SR_ERROR_INVALID_ARGUMENT;
            }

            if (!tmem_resolve_rgba32_address_raw(tile, stride, x, y, &dst) ||
                !tmem_write_be16(tmem, dst.byte, (uint16_t)(texel >> 16)) ||
                !tmem_write_be16(tmem, dst.byte2, (uint16_t)texel)) {
                return SR_ERROR_INVALID_ARGUMENT;
            }
        }
    }

    record_loaded_tile(tmem, tile_index, width, height, stride, sl, tl, sh, th);
    return SR_OK;
}

static sr_result tmem_load_tile_internal(tmem_state *tmem,
                                         sr_memory *memory,
                                         const rdp_state *state,
                                         rdp_metrics *metrics,
                                         const rdp_command *cmd)
{
    if (!tmem || !memory || !state || !cmd) {
        return SR_ERROR_INVALID_ARGUMENT;
    }

    const uint32_t tile_index = cmd->decoded.load.tile_index;
    const rdp_tile *tile = &state->tiles[tile_index];

#if SOFTRDP_ENABLE_PERF_LOG
    if (metrics) {
        switch (cmd->id) {
        case RDP_CMD_LOAD_BLOCK:
            metrics->tex_load_block_count++;
            break;
        case RDP_CMD_LOAD_TLUT:
            metrics->tex_load_tlut_count++;
            break;
        case RDP_CMD_LOAD_TILE:
            metrics->tex_load_tile_count++;
            break;
        default:
            break;
        }
        if (tile->format <= RDP_FORMAT_I && tile->size <= RDP_SIZE_32BPP) {
            metrics->tex_load_by_format_size[tile->format][tile->size]++;
        }
    }
#else
    (void)metrics;
#endif

    if (!texture_state_supports_load(state, tile)) {
        return SR_ERROR_UNSUPPORTED;
    }

    if (cmd->id == RDP_CMD_LOAD_BLOCK) {
        if (tile->size != RDP_SIZE_16BPP) {
            return SR_ERROR_UNSUPPORTED;
        }
        return load_16bpp_block(tmem, memory, state, tile, cmd);
    }

    if (tile->size == RDP_SIZE_32BPP) {
        return load_rgba32_tile(tmem, memory, state, tile, cmd);
    }

    return load_16bpp_tile(tmem, memory, state, tile, cmd);
}

sr_result tmem_load_tile(tmem_state *tmem,
                         sr_memory *memory,
                         const rdp_state *state,
                         rdp_metrics *metrics,
                         const rdp_command *cmd)
{
#if SOFTRDP_ENABLE_PERF_LOG
    LARGE_INTEGER start, end;
    QueryPerformanceCounter(&start);
#endif

    sr_result result = tmem_load_tile_internal(tmem, memory, state, metrics, cmd);

#if SOFTRDP_ENABLE_PERF_LOG
    QueryPerformanceCounter(&end);
    if (metrics) {
        metrics->tex_load_ticks += (end.QuadPart - start.QuadPart);
        metrics->tex_load_count++;
    }
#endif

    return result;
}
