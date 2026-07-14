#include "tmem.h"

#include "rdp_commands.h"
#include "rdp_memory.h"

#include <string.h>

rdp_color tmem_rgba16_decode_table[UINT16_MAX + 1u];

void tmem_init(tmem_state *tmem)
{
    static bool decode_table_ready;
    if (!decode_table_ready) {
        for (uint32_t texel = 0; texel <= UINT16_MAX; texel++)
            tmem_rgba16_decode_table[texel] = pipeline_rgba5551_to_color((uint16_t)texel);
        decode_table_ready = true;
    }
    memset(tmem, 0, sizeof(*tmem));
}

static void record_loaded_tile(tmem_state *tmem, uint32_t tile_index,
                               uint32_t width, uint32_t height, uint32_t stride,
                               uint32_t sl, uint32_t tl, uint32_t sh, uint32_t th);

static bool texture_state_supports_load(const rdp_state *state, const rdp_tile *tile)
{
    if (!state || !tile || state->texture_image.size == RDP_SIZE_4BPP)
        return false;
    if (tile->format == RDP_FORMAT_RGBA && tile->size == RDP_SIZE_32BPP)
        return state->texture_image.size == RDP_SIZE_32BPP;
    return tile->size <= RDP_SIZE_16BPP;
}

static uint32_t texture_bytes_per_texel(rdp_texture_size size)
{
    return size == RDP_SIZE_8BPP ? 1u :
           size == RDP_SIZE_16BPP ? 2u :
           size == RDP_SIZE_32BPP ? 4u : 0u;
}

static uint32_t tile_bits_per_texel(rdp_texture_size size)
{
    return 4u << (uint32_t)size;
}

static sr_result load_generic_tile(tmem_state *tmem, sr_memory *memory,
                                   const rdp_state *state, const rdp_tile *tile,
                                   const rdp_command *cmd)
{
    const uint32_t tile_index = cmd->decoded.load.tile_index;
    const uint32_t sl = cmd->decoded.load.sl >> 2;
    const uint32_t tl = cmd->decoded.load.tl >> 2;
    const uint32_t sh = cmd->decoded.load.sh >> 2;
    const uint32_t th = cmd->decoded.load.th >> 2;
    const uint32_t source_bytes = texture_bytes_per_texel(state->texture_image.size);
    if (!source_bytes || sh < sl || th < tl || tile->size > RDP_SIZE_16BPP)
        return SR_ERROR_INVALID_ARGUMENT;

    const uint32_t source_width = sh - sl + 1u;
    const uint32_t height = th - tl + 1u;
    const uint32_t row_bytes = source_width * source_bytes;
    const uint32_t destination_bits = tile_bits_per_texel(tile->size);
    const uint32_t destination_width = (row_bytes * 8u) / destination_bits;
    const uint32_t stride = tile->line ? tile->line : tmem_align_row_stride(row_bytes);
    if (!destination_width) return SR_ERROR_INVALID_ARGUMENT;

    for (uint32_t y = 0; y < height; y++) {
        const uint32_t source_row = (tl + y) * state->texture_image.width + sl;
        for (uint32_t byte = 0; byte < row_bytes; byte++) {
            uint8_t value;
            tmem_texel_address dst;
            const uint32_t destination_s = tile->size == RDP_SIZE_4BPP ? byte * 2u :
                                           tile->size == RDP_SIZE_8BPP ? byte : byte >> 1;
            if (!sr_memory_read_u8(memory, state->texture_image.address +
                                   source_row * source_bytes + byte, &value) ||
                !tmem_resolve_texel_address_raw(tile, stride, destination_s, y, &dst))
                return SR_ERROR_INVALID_ARGUMENT;
            const uint32_t destination_byte = dst.byte +
                (tile->size == RDP_SIZE_16BPP ? (byte & 1u) : 0u);
            if (destination_byte >= SR_TMEM_SIZE) return SR_ERROR_INVALID_ARGUMENT;
            tmem->bytes[destination_byte] = value;
        }
    }

    if (destination_width == source_width)
        record_loaded_tile(tmem, tile_index, destination_width, height, stride,
                           sl, tl, sh, th);
    else
        record_loaded_tile(tmem, tile_index, destination_width, height, stride,
                           0u, 0u, destination_width - 1u, height - 1u);
    return SR_OK;
}

static sr_result load_8bpp_tile(tmem_state *tmem, sr_memory *memory,
                                const rdp_state *state, const rdp_tile *tile,
                                const rdp_command *cmd)
{
    const uint32_t tile_index = cmd->decoded.load.tile_index;
    const uint32_t sl = cmd->decoded.load.sl >> 2;
    const uint32_t tl = cmd->decoded.load.tl >> 2;
    const uint32_t sh = cmd->decoded.load.sh >> 2;
    const uint32_t th = cmd->decoded.load.th >> 2;
    if (sh < sl || th < tl) return SR_ERROR_INVALID_ARGUMENT;

    const uint32_t width = sh - sl + 1u;
    const uint32_t height = th - tl + 1u;
    const uint32_t stride = tile->line ? tile->line : tmem_align_row_stride(width);
    for (uint32_t y = 0; y < height; y++) {
        for (uint32_t x = 0; x < width; x++) {
            tmem_texel_address dst;
            uint8_t texel;
            const uint32_t src_pixel = (tl + y) * state->texture_image.width + sl + x;
            if (!sr_memory_read_u8(memory, state->texture_image.address + src_pixel, &texel) ||
                !tmem_resolve_texel_address_raw(tile, stride, x, y, &dst) ||
                dst.bytes != 1u || dst.byte >= SR_TMEM_SIZE) {
                return SR_ERROR_INVALID_ARGUMENT;
            }
            tmem->bytes[dst.byte] = texel;
        }
    }

    record_loaded_tile(tmem, tile_index, width, height, stride, sl, tl, sh, th);
    return SR_OK;
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

static sr_result load_generic_block(tmem_state *tmem, sr_memory *memory,
                                    const rdp_state *state, const rdp_tile *tile,
                                    const rdp_command *cmd)
{
    const uint32_t tile_index = cmd->decoded.load.tile_index;
    const uint32_t sl = cmd->decoded.load.sl;
    const uint32_t tl = cmd->decoded.load.tl;
    const uint32_t sh = cmd->decoded.load.sh;
    const uint32_t dxt = cmd->decoded.load.dxt;

    const uint32_t texel_count = ((sh - sl) + 1u) & 0xfffu;
    const uint32_t source_bytes = texture_bytes_per_texel(state->texture_image.size);
    if (texel_count == 0) return SR_OK;
    if (!source_bytes) return SR_ERROR_UNSUPPORTED;

    const uint32_t tmem16_base = tile->tmem >> 1;
    const uint32_t transfer_bytes = texel_count * source_bytes;
    const uint32_t group_count = (transfer_bytes + 7u) >> 3;
    const uint32_t source_pixel = tl * state->texture_image.width + sl;
    const uint32_t source_base = state->texture_image.address + source_pixel * source_bytes;
    uint32_t last_row = 0;

    for (uint32_t group = 0; group < group_count; group++) {
        const uint32_t dxt_row = (group * dxt) >> 11;
        last_row = dxt_row;

        for (uint32_t lane = 0; lane < 4u; lane++) {
            uint8_t high, low;
            const uint32_t source_offset = group * 8u + lane * 2u;
            const uint32_t dst_lane = lane ^ ((dxt_row & 1u) << 1);
            const uint32_t dst_tmem16 = (tmem16_base + (group << 2) + dst_lane) & 0x7ffu;
            const uint32_t dst_addr = (dst_tmem16 ^ 1u) << 1;

            if (!sr_memory_read_u8(memory, source_base + source_offset, &high) ||
                !sr_memory_read_u8(memory, source_base + source_offset + 1u, &low) ||
                !tmem_write_be16(tmem, dst_addr, ((uint16_t)high << 8) | low)) {
                return SR_ERROR_INVALID_ARGUMENT;
            }
        }
    }

    const uint32_t destination_width = (transfer_bytes * 8u) /
                                       tile_bits_per_texel(tile->size);
    record_loaded_tile(tmem, tile_index, destination_width, last_row + 1u, tile->line,
                       0u, 0u, destination_width ? destination_width - 1u : 0u, last_row);
    return SR_OK;
}

static sr_result load_rgba32_block(tmem_state *tmem, sr_memory *memory,
                                   const rdp_state *state, const rdp_tile *tile,
                                   const rdp_command *cmd)
{
    const uint32_t tile_index = cmd->decoded.load.tile_index;
    const uint32_t sl = cmd->decoded.load.sl;
    const uint32_t tl = cmd->decoded.load.tl;
    const uint32_t sh = cmd->decoded.load.sh;
    const uint32_t dxt = cmd->decoded.load.dxt;
    const uint32_t texel_count = ((sh - sl) + 1u) & 0xfffu;
    if (texel_count == 0u) return SR_OK;

    const uint32_t group_count = (texel_count + 1u) >> 1;
    uint32_t last_row = 0u;
    for (uint32_t group = 0; group < group_count; group++) {
        const uint32_t dxt_row = (group * dxt) >> 11;
        last_row = dxt_row;
        for (uint32_t lane = 0; lane < 2u; lane++) {
            const uint32_t x = (group << 1) + lane;
            if (x >= texel_count) break;
            uint32_t texel;
            const uint32_t src_pixel = tl * state->texture_image.width + sl + x;
            if (!sr_memory_read_be32(memory,
                                     state->texture_image.address + src_pixel * 4u,
                                     &texel)) return SR_ERROR_INVALID_ARGUMENT;
            const uint32_t logical = tile->tmem + x * 2u;
            const uint32_t rg = tmem_physical_word_byte(
                logical ^ ((dxt_row & 1u) ? 4u : 0u)) & 0x7ffu;
            if (!tmem_write_be16(tmem, rg, (uint16_t)(texel >> 16)) ||
                !tmem_write_be16(tmem, rg | 0x800u, (uint16_t)texel))
                return SR_ERROR_INVALID_ARGUMENT;
        }
    }
    record_loaded_tile(tmem, tile_index, texel_count, last_row + 1u, tile->line,
                       0u, 0u, texel_count - 1u, last_row);
    return SR_OK;
}

static sr_result load_tlut(tmem_state *tmem, sr_memory *memory,
                           const rdp_state *state, const rdp_tile *tile,
                           const rdp_command *cmd)
{
    if (state->texture_image.size != RDP_SIZE_16BPP) return SR_ERROR_UNSUPPORTED;

    const uint32_t sl = cmd->decoded.load.sl >> 2;
    const uint32_t tl = cmd->decoded.load.tl >> 2;
    const uint32_t sh = cmd->decoded.load.sh >> 2;
    if (sh < sl) return SR_ERROR_INVALID_ARGUMENT;

    const uint32_t count = sh - sl + 1u;
    const uint32_t base = tile->tmem & 0xfffu;
    for (uint32_t i = 0; i < count; i++) {
        uint16_t entry;
        const uint32_t src_pixel = tl * state->texture_image.width + sl + i;
        if (!sr_memory_read_be16(memory, state->texture_image.address + src_pixel * 2u, &entry))
            return SR_ERROR_INVALID_ARGUMENT;

        const uint32_t dst = (base + i * 8u) & 0xfffu;
        for (uint32_t lane = 0; lane < 8u; lane += 2u) {
            if (!tmem_write_be16(tmem, (dst + lane) & 0xfffu, entry))
                return SR_ERROR_INVALID_ARGUMENT;
        }
    }
    tmem->loads_seen++;
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
                                         const rdp_command *cmd)
{
    if (!tmem || !memory || !state || !cmd) {
        return SR_ERROR_INVALID_ARGUMENT;
    }

    const uint32_t tile_index = cmd->decoded.load.tile_index;
    const rdp_tile *tile = &state->tiles[tile_index];

    if (cmd->id == RDP_CMD_LOAD_TLUT) {
        return load_tlut(tmem, memory, state, tile, cmd);
    }

    if (cmd->id == RDP_CMD_LOAD_BLOCK) {
        /* LoadBlock is a transfer operation. The load tile describes the
         * transfer width; a later render tile may reinterpret the same TMEM
         * bytes with a different format or texel size. */
        if (tile->format == RDP_FORMAT_RGBA && tile->size == RDP_SIZE_32BPP &&
            state->texture_image.size == RDP_SIZE_32BPP)
            return load_rgba32_block(tmem, memory, state, tile, cmd);
        if (tile->size <= RDP_SIZE_16BPP)
            return load_generic_block(tmem, memory, state, tile, cmd);
        return SR_ERROR_UNSUPPORTED;
    }

    if (!texture_state_supports_load(state, tile)) {
        return SR_ERROR_UNSUPPORTED;
    }

    if (tile->size == RDP_SIZE_32BPP) {
        return load_rgba32_tile(tmem, memory, state, tile, cmd);
    }

    if (tile->size == RDP_SIZE_8BPP &&
        state->texture_image.size == RDP_SIZE_8BPP) {
        return load_8bpp_tile(tmem, memory, state, tile, cmd);
    }

    if (tile->size == RDP_SIZE_16BPP &&
        state->texture_image.size == RDP_SIZE_16BPP)
        return load_16bpp_tile(tmem, memory, state, tile, cmd);

    return load_generic_tile(tmem, memory, state, tile, cmd);
}

sr_result tmem_load_tile(tmem_state *tmem,
                         sr_memory *memory,
                         const rdp_state *state,
                         const rdp_command *cmd)
{
    return tmem_load_tile_internal(tmem, memory, state, cmd);
}
