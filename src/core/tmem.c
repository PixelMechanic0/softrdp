#include "tmem.h"

#include "rdp_commands.h"
#include "rdp_memory.h"

#include <string.h>

void tmem_init(tmem_state *tmem)
{
    memset(tmem, 0, sizeof(*tmem));
}

sr_result tmem_load_tile(tmem_state *tmem, sr_memory *memory, const rdp_state *state, const rdp_command *cmd)
{
    if (!tmem || !memory || !state || !cmd) {
        return SR_ERROR_INVALID_ARGUMENT;
    }

    const uint32_t tile_index = cmd->decoded.load.tile_index;
    const rdp_tile *tile = &state->tiles[tile_index];
    const uint32_t sl = cmd->decoded.load.sl;
    const uint32_t tl = cmd->decoded.load.tl;
    const uint32_t sh = cmd->decoded.load.sh;
    const uint32_t th = cmd->decoded.load.th;

    if (sh < sl || th < tl) {
        return SR_ERROR_INVALID_ARGUMENT;
    }

    if (state->texture_image.format != RDP_FORMAT_RGBA ||
        state->texture_image.size != RDP_SIZE_16BPP ||
        tile->format != RDP_FORMAT_RGBA ||
        tile->size != RDP_SIZE_16BPP) {
        return SR_ERROR_UNSUPPORTED;
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

    tmem->tile_width[tile_index] = (uint16_t)width;
    tmem->tile_height[tile_index] = (uint16_t)height;
    tmem->tile_stride[tile_index] = (uint16_t)stride;
    tmem->tile_sl[tile_index] = (uint16_t)sl;
    tmem->tile_tl[tile_index] = (uint16_t)tl;
    tmem->tile_sh[tile_index] = (uint16_t)sh;
    tmem->tile_th[tile_index] = (uint16_t)th;
    tmem->loads_seen++;
    return SR_OK;
}
