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

static bool texture_state_supports_rgba16(const rdp_state *state, const rdp_tile *tile)
{
    return state->texture_image.format == RDP_FORMAT_RGBA &&
           state->texture_image.size == RDP_SIZE_16BPP &&
           tile->format == RDP_FORMAT_RGBA &&
           tile->size == RDP_SIZE_16BPP;
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

static sr_result load_rgba16_tile(tmem_state *tmem, sr_memory *memory, const rdp_state *state, const rdp_tile *tile, const rdp_command *cmd)
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
    const uint32_t stride = tile->line ? tile->line : width * 2u;
    const uint32_t dst_base = tile->tmem;

    if (dst_base + (height - 1u) * stride + width * 2u > SR_TMEM_SIZE) {
        return SR_ERROR_INVALID_ARGUMENT;
    }

    for (uint32_t y = 0; y < height; y++) {
        for (uint32_t x = 0; x < width; x++) {
            uint16_t texel;
            const uint32_t src_pixel = (tl + y) * state->texture_image.width + (sl + x);
            const uint32_t src_addr = state->texture_image.address + src_pixel * 2u;
            const uint32_t dst_addr = dst_base + y * stride + x * 2u;

            if (!sr_memory_read_be16(memory, src_addr, &texel)) {
                return SR_ERROR_INVALID_ARGUMENT;
            }

            tmem->bytes[dst_addr + 0u] = (uint8_t)(texel >> 8);
            tmem->bytes[dst_addr + 1u] = (uint8_t)texel;
        }
    }

    record_loaded_tile(tmem, tile_index, width, height, stride, sl, tl, sh, th);
    return SR_OK;
}

static sr_result load_rgba16_block(tmem_state *tmem, sr_memory *memory, const rdp_state *state, const rdp_tile *tile, const rdp_command *cmd)
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

    /*
     * LOAD_BLOCK describes a linear upload plus a DXT stride generator. Most
     * early real-world RGBA16 use is the single-row case; keep that path simple
     * and leave full uneven-DXT reconstruction for the later TMEM-addressing
     * pass rather than baking it into the sampler.
     */
    const uint32_t max_tmem_iteration = (texel_count - 1u) >> 2;
    const uint32_t max_t = dxt ? ((max_tmem_iteration * dxt) >> 11) : 0u;
    if (max_t != 0u) {
        return SR_ERROR_UNSUPPORTED;
    }

    const uint32_t start_s = sl >> 2;
    const uint32_t start_t = tl >> 2;
    uint32_t sample_width = texel_count;
    uint32_t sample_height = 1u;
    if (tile->sh >= tile->sl && tile->th >= tile->tl) {
        const uint32_t tile_width = (tile->sh >> 2) - (tile->sl >> 2) + 1u;
        const uint32_t tile_height = (tile->th >> 2) - (tile->tl >> 2) + 1u;
        if (tile_width > 0u && tile_height > 0u && tile_width * tile_height == texel_count) {
            sample_width = tile_width;
            sample_height = tile_height;
        }
    }

    const uint32_t dst_base = tile->tmem;
    const uint32_t stride = tile->line ? tile->line : sample_width * 2u;

    if (dst_base + (sample_height - 1u) * stride + sample_width * 2u > SR_TMEM_SIZE) {
        return SR_ERROR_INVALID_ARGUMENT;
    }

    for (uint32_t x = 0; x < texel_count; x++) {
        uint16_t texel;
        const uint32_t src_pixel = start_t * state->texture_image.width + start_s + x;
        const uint32_t src_addr = state->texture_image.address + src_pixel * 2u;
        const uint32_t dst_row = x / sample_width;
        const uint32_t dst_col = x - dst_row * sample_width;
        const uint32_t dst_addr = dst_base + dst_row * stride + dst_col * 2u;

        if (!sr_memory_read_be16(memory, src_addr, &texel)) {
            return SR_ERROR_INVALID_ARGUMENT;
        }

        tmem->bytes[dst_addr + 0u] = (uint8_t)(texel >> 8);
        tmem->bytes[dst_addr + 1u] = (uint8_t)texel;
    }

    record_loaded_tile(tmem, tile_index, sample_width, sample_height, stride, 0u, 0u, sample_width - 1u, sample_height - 1u);
    return SR_OK;
}

static sr_result tmem_load_tile_internal(tmem_state *tmem, sr_memory *memory, rdp_state *state, const rdp_command *cmd)
{
    if (!tmem || !memory || !state || !cmd) {
        return SR_ERROR_INVALID_ARGUMENT;
    }

    const uint32_t tile_index = cmd->decoded.load.tile_index;
    const rdp_tile *tile = &state->tiles[tile_index];

    if (!texture_state_supports_rgba16(state, tile)) {
        return SR_ERROR_UNSUPPORTED;
    }

    if (cmd->id == RDP_CMD_LOAD_BLOCK) {
        return load_rgba16_block(tmem, memory, state, tile, cmd);
    }

    return load_rgba16_tile(tmem, memory, state, tile, cmd);
}

sr_result tmem_load_tile(tmem_state *tmem, sr_memory *memory, rdp_state *state, const rdp_command *cmd)
{
#if SOFTRDP_ENABLE_PERF_LOG
    LARGE_INTEGER start, end;
    QueryPerformanceCounter(&start);
#endif

    sr_result result = tmem_load_tile_internal(tmem, memory, state, cmd);

#if SOFTRDP_ENABLE_PERF_LOG
    QueryPerformanceCounter(&end);
    if (state) {
        state->tex_load_ticks += (end.QuadPart - start.QuadPart);
        state->tex_load_count++;
    }
#endif

    return result;
}
