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
    const uint32_t tile_index = (cmd->words[1] >> 24) & 7u;
    const rdp_tile *tile = &state->tiles[tile_index];
    const uint32_t sl = ((cmd->words[0] >> 12) & 0xfffu) >> 2;
    const uint32_t tl = (cmd->words[0] & 0xfffu) >> 2;
    const uint32_t sh = ((cmd->words[1] >> 12) & 0xfffu) >> 2;
    const uint32_t th = (cmd->words[1] & 0xfffu) >> 2;

    if (!tmem || !memory || !state || sh < sl || th < tl) {
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

bool tmem_sample_rgba5551(const tmem_state *tmem, const rdp_state *state, uint32_t tile_index, uint32_t s, uint32_t t, uint16_t *texel)
{
    if (!tmem || !state || !texel || tile_index >= 8 ||
        tmem->tile_width[tile_index] == 0 || tmem->tile_height[tile_index] == 0) {
        return false;
    }

    const rdp_tile *tile = &state->tiles[tile_index];
    uint32_t tile_sl = tmem->tile_sl[tile_index];
    uint32_t tile_tl = tmem->tile_tl[tile_index];
    uint32_t tile_sh = tmem->tile_sh[tile_index];
    uint32_t tile_th = tmem->tile_th[tile_index];
    uint32_t local_s;
    uint32_t local_t;

    if (tile->sh > tile->sl || tile->th > tile->tl) {
        tile_sl = tile->sl >> 2;
        tile_tl = tile->tl >> 2;
        tile_sh = tile->sh >> 2;
        tile_th = tile->th >> 2;
    }

    if (s < tile_sl) {
        if (!tile->clamp_s) {
            return false;
        }
        local_s = 0;
    } else if (s > tile_sh) {
        if (!tile->clamp_s) {
            return false;
        }
        local_s = tmem->tile_width[tile_index] - 1u;
    } else {
        local_s = s - tile_sl;
    }

    if (t < tile_tl) {
        if (!tile->clamp_t) {
            return false;
        }
        local_t = 0;
    } else if (t > tile_th) {
        if (!tile->clamp_t) {
            return false;
        }
        local_t = tmem->tile_height[tile_index] - 1u;
    } else {
        local_t = t - tile_tl;
    }

    if (local_s >= tmem->tile_width[tile_index] || local_t >= tmem->tile_height[tile_index]) {
        return false;
    }

    const uint32_t addr = tile->tmem + local_t * tmem->tile_stride[tile_index] + local_s * 2u;
    if (addr + 1u >= SR_TMEM_SIZE) {
        return false;
    }

    *texel = ((uint16_t)tmem->bytes[addr] << 8) | (uint16_t)tmem->bytes[addr + 1u];
    return true;
}
