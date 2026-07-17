#ifndef TMEM_H
#define TMEM_H

#include "rdp_state.h"
#include <stdbool.h>
#include <stdint.h>

typedef struct rdp_command rdp_command;
typedef struct sr_memory sr_memory;

typedef struct tmem_state {
    uint8_t bytes[SR_TMEM_SIZE];
    uint16_t tile_width[8];
    uint16_t tile_height[8];
    uint16_t tile_stride[8];
    uint16_t tile_sl[8];
    uint16_t tile_tl[8];
    uint16_t tile_sh[8];
    uint16_t tile_th[8];
    uint64_t loads_seen;
} tmem_state;

typedef struct tmem_texel_address {
    uint32_t byte;
    uint32_t byte2;
    uint8_t subtexel;
    uint8_t bytes;
} tmem_texel_address;

extern rdp_color tmem_rgba16_decode_table[UINT16_MAX + 1u];

void tmem_init(tmem_state *tmem);
sr_result tmem_load_tile(tmem_state *tmem, sr_memory *memory, const rdp_state *state, const rdp_command *cmd);

static inline int32_t sign16_coord(int32_t value)
{
    return (int16_t)(value & 0xffff);
}

static inline int32_t shift_tile_coord(int32_t coord, uint8_t shift)
{
    if (shift < 11u) {
        return sign16_coord(coord) >> shift;
    }
    if (shift < 16u) {
        return sign16_coord((int32_t)((uint32_t)coord << (16u - shift)));
    }
    return sign16_coord(coord);
}

static inline int32_t shift_tile_coord_fixed5(int32_t coord, uint8_t shift)
{
    if (shift < 11u) {
        return sign16_coord(coord) >> shift;
    }
    if (shift < 16u) {
        return sign16_coord((int32_t)((uint32_t)coord << (16u - shift)));
    }
    return sign16_coord(coord);
}

static inline uint32_t tmem_align_row_stride(uint32_t bytes)
{
    return (bytes + 7u) & ~7u;
}

static inline uint32_t tmem_physical_byte(uint32_t byte)
{
    /* TMEM words are stored in big-endian byte order in this representation.
     * Word addressing already swaps adjacent 16-bit lanes, so byte textures
     * select within the resulting word with XOR 2. */
    return byte ^ 2u;
}

static inline uint32_t tmem_physical_word_byte(uint32_t byte)
{
    return ((byte >> 1) ^ 1u) << 1;
}

static inline int32_t fixed5_floor_to_texel(int32_t coord)
{
    const int64_t value = coord;
    return value >= 0 ? (int32_t)(value / 32) : (int32_t)-(((-value) + 31) / 32);
}

static inline bool resolve_tile_axis(int32_t coord,
                                     int32_t lo,
                                     int32_t hi,
                                     uint32_t extent,
                                     bool clamp,
                                     bool mirror,
                                     uint8_t mask_bits,
                                     uint32_t *local)
{
    int32_t relative;

    if (clamp) {
        if (coord < lo) {
            relative = 0;
        } else if (coord > hi) {
            relative = (int32_t)extent - 1;
        } else {
            relative = coord - lo;
        }
    } else {
        relative = coord - lo;
    }

    if (mask_bits) {
        const int32_t period = 1 << mask_bits;
        const int32_t repeat_mask = mirror ? (period << 1) - 1 : period - 1;
        relative &= repeat_mask;
        if (mirror) {
            if (relative & period) {
                relative = ((period << 1) - 1) - relative;
            }
        }
    } else if (!clamp && (relative < 0 || relative >= (int32_t)extent)) {
        return false;
    }

    if (relative < 0 || relative >= (int32_t)extent) {
        return false;
    }

    *local = (uint32_t)relative;
    return true;
}

static inline bool tmem_resolve_rgba16_address_raw(const rdp_tile *tile,
                                                   uint32_t stride,
                                                   uint32_t local_s,
                                                   uint32_t local_t,
                                                   tmem_texel_address *address)
{
    if (!tile || !address || stride == 0) {
        return false;
    }

    /*
     * RGBA16 fetches words from TMEM in four-word groups. Odd rows flip word
     * address bit 1, which is the first piece of the real TMEM lane addressing
     * model that matters for nearest RGBA16 sampling.
     */
    const uint32_t logical = tile->tmem + local_t * stride + local_s * 2u;
    address->byte = tmem_physical_word_byte(logical ^ ((local_t & 1u) ? 4u : 0u));
    address->byte2 = 0;
    address->subtexel = 0;
    address->bytes = 2;
    return address->byte + 1u < SR_TMEM_SIZE;
}

static inline bool tmem_resolve_rgba32_address_raw(const rdp_tile *tile,
                                                   uint32_t stride,
                                                   uint32_t local_s,
                                                   uint32_t local_t,
                                                   tmem_texel_address *address)
{
    if (!tile || !address || stride == 0) {
        return false;
    }

    const uint32_t logical = tile->tmem + local_t * stride + local_s * 2u;
    /* RGBA32 is split across the two 2 KiB TMEM banks. The tile address
     * selects within a bank; it must not linearly run past the end of TMEM. */
    address->byte = tmem_physical_word_byte(
        logical ^ ((local_t & 1u) ? 4u : 0u)) & 0x7ffu;
    address->byte2 = address->byte | 0x800u;
    address->subtexel = 0;
    address->bytes = 4;
    return true;
}

static inline bool tmem_tile_extent_from_descriptor(const rdp_tile *tile, uint32_t *width, uint32_t *height)
{
    if (!tile || !width || !height) {
        return false;
    }

    *width = (((tile->sh >> 2) - (tile->sl >> 2)) & 0x3ffu) + 1u;
    *height = (((tile->th >> 2) - (tile->tl >> 2)) & 0x3ffu) + 1u;
    return *width != 0 && *height != 0;
}

static inline uint32_t tmem_derived_row_stride(const rdp_tile *tile, uint32_t width)
{
    const uint32_t bytes_per_texel = tile->size == RDP_SIZE_32BPP ? 4u :
                                     tile->size == RDP_SIZE_16BPP ? 2u :
                                     tile->size == RDP_SIZE_8BPP ? 1u : 0u;
    const uint32_t row_bytes = bytes_per_texel ? width * bytes_per_texel
                                               : (width + 1u) >> 1;
    return tmem_align_row_stride(row_bytes);
}

static inline bool tmem_tile_sample_layout(const tmem_state *tmem,
                                           const rdp_texture_sample_state *sample,
                                           uint32_t *width,
                                           uint32_t *height,
                                           uint32_t *stride)
{
    if (!tmem || !sample || !width || !height || !stride || sample->tile_index >= 8) {
        return false;
    }

    const uint32_t tile_index = sample->tile_index;
    const rdp_tile *tile = &sample->tile;
    if (sample->width != 0 && sample->height != 0 && sample->stride != 0) {
        *width = sample->width;
        *height = sample->height;
        *stride = sample->stride;
        return true;
    }
    const bool single_descriptor_texel = tile->sh == tile->sl && tile->th == tile->tl;
    if (!single_descriptor_texel && tmem_tile_extent_from_descriptor(tile, width, height)) {
        *stride = tile->line;
        if (*stride == 0) *stride = tmem_derived_row_stride(tile, *width);
    } else if (tmem->tile_width[tile_index] != 0 && tmem->tile_height[tile_index] != 0) {
        *width = tmem->tile_width[tile_index];
        *height = tmem->tile_height[tile_index];
        *stride = tmem->tile_stride[tile_index];
    } else if (tmem_tile_extent_from_descriptor(tile, width, height)) {
        *stride = tile->line ? tile->line : tmem_derived_row_stride(tile, *width);
    } else {
        return false;
    }

    return *stride != 0;
}

static inline bool tmem_resolve_texel_address_raw(const rdp_tile *tile,
                                                  uint32_t stride,
                                                  uint32_t local_s,
                                                  uint32_t local_t,
                                                  tmem_texel_address *address)
{
    if (!tile || !address || stride == 0) {
        return false;
    }

    const uint32_t row_xor = (local_t & 1u) ? 4u : 0u;
    switch (tile->size) {
    case RDP_SIZE_4BPP:
        address->byte = tmem_physical_byte(tile->tmem + local_t * stride + ((local_s >> 1) ^ row_xor));
        address->byte2 = 0;
        address->subtexel = (uint8_t)(local_s & 1u);
        address->bytes = 1;
        return address->byte < SR_TMEM_SIZE;
    case RDP_SIZE_8BPP:
        address->byte = tmem_physical_byte(tile->tmem + local_t * stride + (local_s ^ row_xor));
        address->byte2 = 0;
        address->subtexel = 0;
        address->bytes = 1;
        return address->byte < SR_TMEM_SIZE;
    case RDP_SIZE_16BPP:
        return tmem_resolve_rgba16_address_raw(tile, stride, local_s, local_t, address);
    case RDP_SIZE_32BPP:
        return tmem_resolve_rgba32_address_raw(tile, stride, local_s, local_t, address);
    }

    return false;
}

static inline bool tmem_resolve_texel_coord(const tmem_state *tmem,
                                            const rdp_texture_sample_state *sample,
                                            int32_t s,
                                            int32_t t,
                                            uint32_t *local_s,
                                            uint32_t *local_t)
{
    if (!tmem || !sample || !local_s || !local_t || sample->tile_index >= 8) {
        return false;
    }

    const rdp_tile *tile = &sample->tile;
    const rdp_tile_bounds *bounds = &sample->bounds;
    uint32_t width;
    uint32_t height;
    uint32_t stride;
    if (!tmem_tile_sample_layout(tmem, sample, &width, &height, &stride)) {
        return false;
    }

    s = shift_tile_coord(s, tile->shift_s);
    t = shift_tile_coord(t, tile->shift_t);

    return resolve_tile_axis(s,
                             bounds->sl,
                             bounds->sh,
                             width,
                             tile->clamp_s != 0 || tile->mask_s == 0,
                             tile->mirror_s != 0,
                             tile->mask_s,
                             local_s) &&
           resolve_tile_axis(t,
                             bounds->tl,
                             bounds->th,
                             height,
                             tile->clamp_t != 0 || tile->mask_t == 0,
                             tile->mirror_t != 0,
                             tile->mask_t,
                             local_t);
}

static inline bool tmem_resolve_texel_coord_fixed5(const tmem_state *tmem,
                                                   const rdp_texture_sample_state *sample,
                                                   int32_t s_fixed,
                                                   int32_t t_fixed,
                                                   uint32_t *local_s,
                                                   uint32_t *local_t)
{
    if (!tmem || !sample || !local_s || !local_t || sample->tile_index >= 8) {
        return false;
    }

    const rdp_tile *tile = &sample->tile;
    const rdp_tile_bounds *bounds = &sample->bounds;
    uint32_t width;
    uint32_t height;
    uint32_t stride;
    if (!tmem_tile_sample_layout(tmem, sample, &width, &height, &stride)) {
        return false;
    }
    (void)stride;

    s_fixed = shift_tile_coord_fixed5(s_fixed, tile->shift_s);
    t_fixed = shift_tile_coord_fixed5(t_fixed, tile->shift_t);

    const int32_t s_relative = s_fixed - ((int32_t)tile->sl << 3);
    const int32_t t_relative = t_fixed - ((int32_t)tile->tl << 3);
    const int32_t s_texel = fixed5_floor_to_texel(s_relative);
    const int32_t t_texel = fixed5_floor_to_texel(t_relative);

    return resolve_tile_axis(s_texel,
                             0,
                             bounds->sh >= bounds->sl ? (int32_t)(bounds->sh - bounds->sl) : 0,
                             width,
                             tile->clamp_s != 0 || tile->mask_s == 0,
                             tile->mirror_s != 0,
                             tile->mask_s,
                             local_s) &&
           resolve_tile_axis(t_texel,
                             0,
                             bounds->th >= bounds->tl ? (int32_t)(bounds->th - bounds->tl) : 0,
                             height,
                             tile->clamp_t != 0 || tile->mask_t == 0,
                             tile->mirror_t != 0,
                             tile->mask_t,
                             local_t);
}

static inline bool tmem_resolve_texel_address(const tmem_state *tmem,
                                              const rdp_texture_sample_state *sample,
                                              uint32_t local_s,
                                              uint32_t local_t,
                                              tmem_texel_address *address)
{
    if (!tmem || !sample || !address || sample->tile_index >= 8) {
        return false;
    }

    const rdp_tile *tile = &sample->tile;
    uint32_t width;
    uint32_t height;
    uint32_t stride;
    if (!tmem_tile_sample_layout(tmem, sample, &width, &height, &stride)) {
        return false;
    }

    if (local_s >= width || local_t >= height) {
        return false;
    }

    return tmem_resolve_texel_address_raw(tile,
                                          stride,
                                          local_s,
                                          local_t,
                                          address);
}

static inline bool tmem_sample_rgba5551(const tmem_state *tmem, const rdp_texture_sample_state *sample, int32_t s, int32_t t, uint16_t *texel)
{
    if (!texel) {
        return false;
    }

    uint32_t local_s;
    uint32_t local_t;
    if (!tmem_resolve_texel_coord(tmem, sample, s, t, &local_s, &local_t)) {
        return false;
    }

    if (!sample || sample->tile.format != RDP_FORMAT_RGBA ||
        sample->tile.size != RDP_SIZE_16BPP) {
        return false;
    }

    tmem_texel_address address;
    if (!tmem_resolve_texel_address(tmem, sample, local_s, local_t, &address)) {
        return false;
    }

    if (address.bytes != 2) {
        return false;
    }

    *texel = ((uint16_t)tmem->bytes[address.byte] << 8) | (uint16_t)tmem->bytes[address.byte + 1u];
    return true;
}

static inline uint8_t expand_4_to_8(uint32_t value)
{
    value &= 0xfu;
    return (uint8_t)((value << 4) | value);
}

static inline uint8_t lerp_u8(uint8_t a, uint8_t b, uint32_t frac5)
{
    return (uint8_t)(((uint32_t)a * (32u - frac5) + (uint32_t)b * frac5 + 16u) >> 5);
}

static inline rdp_color lerp_color(rdp_color a, rdp_color b, uint32_t frac5)
{
    return (rdp_color){
        lerp_u8(a.r, b.r, frac5),
        lerp_u8(a.g, b.g, frac5),
        lerp_u8(a.b, b.b, frac5),
        lerp_u8(a.a, b.a, frac5)
    };
}

static inline uint8_t clamp_i32_to_u8(int32_t value)
{
    return value < 0 ? 0u : (value > 255 ? 255u : (uint8_t)value);
}

static inline __attribute__((always_inline)) uint8_t bilerp_3tap_u8(uint8_t base, uint8_t edge_s, uint8_t edge_t, uint32_t frac_s, uint32_t frac_t)
{
    int32_t value = ((int32_t)edge_s - (int32_t)base) * (int32_t)frac_s;
    value += ((int32_t)edge_t - (int32_t)base) * (int32_t)frac_t;
    value = (value + 0x10) >> 5;
    value += base;
    return clamp_i32_to_u8(value);
}

static inline __attribute__((always_inline)) rdp_color bilerp_3tap_color(rdp_color base, rdp_color edge_s, rdp_color edge_t, uint32_t frac_s, uint32_t frac_t)
{
    return (rdp_color){
        bilerp_3tap_u8(base.r, edge_s.r, edge_t.r, frac_s, frac_t),
        bilerp_3tap_u8(base.g, edge_s.g, edge_t.g, frac_s, frac_t),
        bilerp_3tap_u8(base.b, edge_s.b, edge_t.b, frac_s, frac_t),
        bilerp_3tap_u8(base.a, edge_s.a, edge_t.a, frac_s, frac_t)
    };
}

static inline bool tmem_fetch_color_local(const tmem_state *tmem,
                                          const rdp_texture_sample_state *sample,
                                          uint32_t local_s,
                                          uint32_t local_t,
                                          rdp_color *color)
{
    if (!color || !sample || sample->tile_index >= 8) {
        return false;
    }

    const rdp_tile *tile = &sample->tile;
    tmem_texel_address address;
    const bool resolved = sample->width && sample->height && sample->stride
        ? (local_s < sample->width && local_t < sample->height &&
           tmem_resolve_texel_address_raw(&sample->tile, sample->stride,
                                          local_s, local_t, &address))
        : tmem_resolve_texel_address(tmem, sample, local_s, local_t, &address);
    if (!resolved) {
        return false;
    }

    /* TLUT-enabled IA16 rectangle effects use the upper texel byte as their
     * palette index. */
    const bool indexed_tlut = sample->tlut_enable &&
        tile->format != RDP_FORMAT_YUV &&
        (tile->size == RDP_SIZE_4BPP || tile->size == RDP_SIZE_8BPP ||
         (sample->tlut_wide_index && tile->size == RDP_SIZE_16BPP));
    if (indexed_tlut) {
        uint32_t index;
        if (tile->size == RDP_SIZE_4BPP && address.bytes == 1) {
            const uint8_t packed = tmem->bytes[address.byte];
            index = ((uint32_t)tile->palette << 4) |
                    (address.subtexel ? (packed & 0xfu) : (packed >> 4));
        } else if (tile->size == RDP_SIZE_8BPP && address.bytes == 1) {
            index = tmem->bytes[address.byte];
        } else if (tile->size == RDP_SIZE_16BPP && address.bytes == 2) {
            index = tmem->bytes[address.byte];
        } else {
            return false;
        }

        const uint32_t palette_logical = 0x800u + index * 8u;
        const uint32_t palette_addr = tile->size == RDP_SIZE_16BPP
            ? tmem_physical_word_byte(palette_logical) : palette_logical;
        if (palette_addr + 1u >= SR_TMEM_SIZE) return false;
        const uint16_t entry = ((uint16_t)tmem->bytes[palette_addr] << 8) |
                               (uint16_t)tmem->bytes[palette_addr + 1u];
        if (sample->tlut_ia) {
            const uint8_t intensity = (uint8_t)(entry >> 8);
            *color = (rdp_color){ intensity, intensity, intensity, (uint8_t)entry };
        } else {
            *color = pipeline_rgba5551_to_color(entry);
        }
        return true;
    }

    if (tile->format == RDP_FORMAT_IA) {
        switch (tile->size) {
        case RDP_SIZE_4BPP: {
            if (address.bytes != 1) {
                return false;
            }
            const uint8_t packed = tmem->bytes[address.byte];
            const uint8_t texel = address.subtexel ? (packed & 0xfu) : (packed >> 4);
            const uint8_t intensity_bits = texel & 0xeu;
            const uint8_t intensity = (uint8_t)((intensity_bits << 4) |
                                                (intensity_bits << 1) |
                                                (intensity_bits >> 2));
            const uint8_t alpha = (texel & 1u) ? 0xffu : 0u;
            *color = (rdp_color){ intensity, intensity, intensity, alpha };
            return true;
        }
        case RDP_SIZE_8BPP: {
            if (address.bytes != 1) {
                return false;
            }
            const uint8_t texel = tmem->bytes[address.byte];
            const uint8_t intensity = expand_4_to_8(texel >> 4);
            const uint8_t alpha = expand_4_to_8(texel);
            *color = (rdp_color){ intensity, intensity, intensity, alpha };
            return true;
        }
        case RDP_SIZE_16BPP:
            if (address.bytes != 2) {
                return false;
            }
            *color = (rdp_color){
                tmem->bytes[address.byte],
                tmem->bytes[address.byte],
                tmem->bytes[address.byte],
                tmem->bytes[address.byte + 1u]
            };
            return true;
        default:
            return false;
        }
    }

    if (tile->format == RDP_FORMAT_I) {
        if (tile->size == RDP_SIZE_4BPP && address.bytes == 1) {
            const uint8_t packed = tmem->bytes[address.byte];
            const uint8_t nibble = address.subtexel ? (packed & 0xfu) : (packed >> 4);
            const uint8_t intensity = expand_4_to_8(nibble);
            *color = (rdp_color){ intensity, intensity, intensity, intensity };
            return true;
        }
        if (tile->size == RDP_SIZE_8BPP && address.bytes == 1) {
            const uint8_t intensity = tmem->bytes[address.byte];
            *color = (rdp_color){ intensity, intensity, intensity, intensity };
            return true;
        }
        return false;
    }

    if (tile->format == RDP_FORMAT_YUV) {
        if (tile->size != RDP_SIZE_16BPP) {
            return false;
        }

        tmem_texel_address addr_y;
        if (!tmem_resolve_texel_address_raw(tile, sample->stride, local_s, local_t, &addr_y)) {
            return false;
        }

        tmem_texel_address addr_u;
        if (!tmem_resolve_texel_address_raw(tile, sample->stride, local_s & ~1u, local_t, &addr_u)) {
            return false;
        }

        tmem_texel_address addr_v;
        if (!tmem_resolve_texel_address_raw(tile, sample->stride, local_s | 1u, local_t, &addr_v)) {
            return false;
        }

        uint16_t word_y = ((uint16_t)tmem->bytes[addr_y.byte] << 8) | (uint16_t)tmem->bytes[addr_y.byte + 1u];
        uint16_t word_u = ((uint16_t)tmem->bytes[addr_u.byte] << 8) | (uint16_t)tmem->bytes[addr_u.byte + 1u];
        uint16_t word_v = ((uint16_t)tmem->bytes[addr_v.byte] << 8) | (uint16_t)tmem->bytes[addr_v.byte + 1u];

        uint8_t y = (uint8_t)(word_y >> 8);
        int32_t u = (int32_t)(word_u & 0xffu) - 128;
        int32_t v = (int32_t)(word_v & 0xffu) - 128;

        if (sample->convert_one) {
            int32_t r_val = (int32_t)y + ((sample->convert_k0_tf * v + 0x80) >> 8);
            int32_t g_val = (int32_t)y + ((sample->convert_k1_tf * u + sample->convert_k2_tf * v + 0x80) >> 8);
            int32_t b_val = (int32_t)y + ((sample->convert_k3_tf * u + 0x80) >> 8);
            color->r = r_val < 0 ? 0 : (r_val > 255 ? 255 : (uint8_t)r_val);
            color->g = g_val < 0 ? 0 : (g_val > 255 ? 255 : (uint8_t)g_val);
            color->b = b_val < 0 ? 0 : (b_val > 255 ? 255 : (uint8_t)b_val);
            color->a = y;
        } else {
            color->r = (uint8_t)u;
            color->g = (uint8_t)v;
            color->b = y;
            color->a = y;
        }
        return true;
    }

    if (tile->format != RDP_FORMAT_RGBA) {
        return false;
    }

    switch (tile->size) {
    case RDP_SIZE_16BPP: {
        if (address.bytes != 2) {
            return false;
        }
        const uint16_t texel = ((uint16_t)tmem->bytes[address.byte] << 8) | (uint16_t)tmem->bytes[address.byte + 1u];
        *color = pipeline_rgba5551_to_color(texel);
        return true;
    }
    case RDP_SIZE_32BPP:
        if (address.bytes != 4 || address.byte + 1u >= SR_TMEM_SIZE || address.byte2 + 1u >= SR_TMEM_SIZE) {
            return false;
        }
        *color = (rdp_color){
            tmem->bytes[address.byte],
            tmem->bytes[address.byte + 1u],
            tmem->bytes[address.byte2],
            tmem->bytes[address.byte2 + 1u]
        };
        return true;
    default:
        return false;
    }
}

static inline bool tmem_sample_color(const tmem_state *tmem, const rdp_texture_sample_state *sample, int32_t s, int32_t t, rdp_color *color)
{
    if (!color || !sample || sample->tile_index >= 8) {
        return false;
    }

    uint32_t local_s;
    uint32_t local_t;
    if (!tmem_resolve_texel_coord(tmem, sample, s, t, &local_s, &local_t)) {
        return false;
    }

    return tmem_fetch_color_local(tmem, sample, local_s, local_t, color);
}

static inline bool tmem_resolve_compiled_axis_fixed5(const rdp_texture_sample_state *sample,
                                                      int32_t fixed, bool s_axis,
                                                      uint32_t *local)
{
    if (!sample || !local || !sample->width || !sample->height || !sample->stride) return false;
    const rdp_tile *tile = &sample->tile;
    const uint8_t shift = s_axis ? tile->shift_s : tile->shift_t;
    const int32_t origin = (int32_t)(s_axis ? tile->sl : tile->tl) << 3;
    const uint32_t bound_lo = s_axis ? sample->bounds.sl : sample->bounds.tl;
    const uint32_t bound_hi = s_axis ? sample->bounds.sh : sample->bounds.th;
    const uint32_t extent = s_axis ? sample->width : sample->height;
    const uint8_t mask = s_axis ? tile->mask_s : tile->mask_t;
    const int32_t relative = fixed5_floor_to_texel(
        shift_tile_coord_fixed5(fixed, shift) - origin);
    return resolve_tile_axis(relative, 0, bound_hi >= bound_lo ? (int32_t)(bound_hi - bound_lo) : 0,
                             extent, (s_axis ? tile->clamp_s : tile->clamp_t) != 0 || mask == 0,
                             (s_axis ? tile->mirror_s : tile->mirror_t) != 0, mask, local);
}

static inline bool tmem_resolve_compiled_coord_fixed5(const rdp_texture_sample_state *sample,
                                                       int32_t s_fixed, int32_t t_fixed,
                                                       uint32_t *local_s, uint32_t *local_t)
{
    return tmem_resolve_compiled_axis_fixed5(sample, s_fixed, true, local_s) &&
           tmem_resolve_compiled_axis_fixed5(sample, t_fixed, false, local_t);
}

static inline bool tmem_sample_point_compiled_fixed5(const tmem_state *tmem,
                                                      const rdp_texture_sample_state *sample,
                                                      int32_t s_fixed, int32_t t_fixed,
                                                      rdp_color *color)
{
    uint32_t local_s, local_t;
    return color && tmem_resolve_compiled_coord_fixed5(sample, s_fixed, t_fixed,
                                                        &local_s, &local_t) &&
           tmem_fetch_color_local(tmem, sample, local_s, local_t, color);
}

static inline bool tmem_fetch_rgba16_local(const tmem_state *tmem,
                                            const rdp_texture_sample_state *sample,
                                            uint32_t local_s, uint32_t local_t,
                                            rdp_color *color)
{
    tmem_texel_address address;
    if (!tmem_resolve_rgba16_address_raw(&sample->tile, sample->stride,
                                         local_s, local_t, &address) ||
        address.byte + 1u >= SR_TMEM_SIZE) return false;
    const uint16_t texel = ((uint16_t)tmem->bytes[address.byte] << 8) |
                           (uint16_t)tmem->bytes[address.byte + 1u];
    *color = tmem_rgba16_decode_table[texel];
    return true;
}

static inline bool tmem_sample_rgba16_point_fixed5(const tmem_state *tmem,
                                                    const rdp_texture_sample_state *sample,
                                                    int32_t s_fixed, int32_t t_fixed,
                                                    rdp_color *color)
{
    uint32_t local_s, local_t;
    return color && tmem_resolve_compiled_coord_fixed5(sample, s_fixed, t_fixed,
                                                        &local_s, &local_t) &&
           tmem_fetch_rgba16_local(tmem, sample, local_s, local_t, color);
}

static inline bool tmem_fetch_compact_local(const tmem_state *tmem,
                                            const rdp_texture_sample_state *sample,
                                            uint32_t local_s, uint32_t local_t,
                                            rdp_color *color)
{
    tmem_texel_address address;
    if (!tmem_resolve_texel_address_raw(&sample->tile, sample->stride,
                                        local_s, local_t, &address) ||
        address.byte >= SR_TMEM_SIZE || address.bytes != 1u) return false;
    if (sample->sampler_class == RDP_SAMPLER_I4_BILERP) {
        const uint8_t packed = tmem->bytes[address.byte];
        const uint8_t intensity = expand_4_to_8(
            address.subtexel ? (packed & 0xfu) : (packed >> 4));
        *color = (rdp_color){ intensity, intensity, intensity, intensity };
        return true;
    }
    if (sample->sampler_class == RDP_SAMPLER_I8_BILERP) {
        const uint8_t intensity = tmem->bytes[address.byte];
        *color = (rdp_color){ intensity, intensity, intensity, intensity };
        return true;
    }
    if (sample->sampler_class == RDP_SAMPLER_IA8_BILERP) {
        const uint8_t texel = tmem->bytes[address.byte];
        const uint8_t intensity = expand_4_to_8(texel >> 4);
        const uint8_t alpha = expand_4_to_8(texel);
        *color = (rdp_color){ intensity, intensity, intensity, alpha };
        return true;
    }
    const uint32_t palette_addr = 0x800u + (uint32_t)tmem->bytes[address.byte] * 8u;
    if (palette_addr + 1u >= SR_TMEM_SIZE) return false;
    const uint16_t entry = ((uint16_t)tmem->bytes[palette_addr] << 8) |
                           (uint16_t)tmem->bytes[palette_addr + 1u];
    if (sample->tlut_ia) {
        const uint8_t intensity = (uint8_t)(entry >> 8);
        *color = (rdp_color){ intensity, intensity, intensity, (uint8_t)entry };
    } else {
        *color = tmem_rgba16_decode_table[entry];
    }
    return true;
}

static inline bool tmem_sample_compact_bilerp_fixed5(const tmem_state *tmem,
                                                     const rdp_texture_sample_state *sample,
                                                     int32_t s_fixed, int32_t t_fixed,
                                                     rdp_color *color)
{
    uint32_t s0, t0, s1, t1;
    if (!color || !tmem_resolve_compiled_axis_fixed5(sample, s_fixed, true, &s0) ||
        !tmem_resolve_compiled_axis_fixed5(sample, s_fixed + 32, true, &s1) ||
        !tmem_resolve_compiled_axis_fixed5(sample, t_fixed, false, &t0) ||
        !tmem_resolve_compiled_axis_fixed5(sample, t_fixed + 32, false, &t1))
        return false;
    const rdp_tile *tile = &sample->tile;
    const int32_t shifted_s = shift_tile_coord_fixed5(s_fixed, tile->shift_s);
    const int32_t shifted_t = shift_tile_coord_fixed5(t_fixed, tile->shift_t);
    const uint32_t frac_s = (uint32_t)(shifted_s - ((int32_t)tile->sl << 3)) & 31u;
    const uint32_t frac_t = (uint32_t)(shifted_t - ((int32_t)tile->tl << 3)) & 31u;
    rdp_color c00, c10, c01, c11;
    if (sample->mid_texel && frac_s == 16u && frac_t == 16u) {
        if (tmem_fetch_compact_local(tmem, sample, s0, t0, &c00) &&
            tmem_fetch_compact_local(tmem, sample, s1, t0, &c10) &&
            tmem_fetch_compact_local(tmem, sample, s0, t1, &c01) &&
            tmem_fetch_compact_local(tmem, sample, s1, t1, &c11)) {
            *color = (rdp_color){
                (uint8_t)(((uint32_t)c00.r + c10.r + c01.r + c11.r + 2u) >> 2),
                (uint8_t)(((uint32_t)c00.g + c10.g + c01.g + c11.g + 2u) >> 2),
                (uint8_t)(((uint32_t)c00.b + c10.b + c01.b + c11.b + 2u) >> 2),
                (uint8_t)(((uint32_t)c00.a + c10.a + c01.a + c11.a + 2u) >> 2)
            };
            return true;
        }
    } else if (frac_s + frac_t >= 32u) {
        if (tmem_fetch_compact_local(tmem, sample, s1, t1, &c11) &&
            tmem_fetch_compact_local(tmem, sample, s1, t0, &c10) &&
            tmem_fetch_compact_local(tmem, sample, s0, t1, &c01)) {
            *color = bilerp_3tap_color(c11, c10, c01, 32u - frac_t, 32u - frac_s);
            return true;
        }
    } else {
        if (tmem_fetch_compact_local(tmem, sample, s0, t0, &c00) &&
            tmem_fetch_compact_local(tmem, sample, s1, t0, &c10) &&
            tmem_fetch_compact_local(tmem, sample, s0, t1, &c01)) {
            *color = bilerp_3tap_color(c00, c10, c01, frac_s, frac_t);
            return true;
        }
    }
    return tmem_fetch_compact_local(tmem, sample, s0, t0, color);
}

static inline bool tmem_sample_rgba16_bilerp_fixed5(const tmem_state *tmem,
                                                     const rdp_texture_sample_state *sample,
                                                     int32_t s_fixed, int32_t t_fixed,
                                                     rdp_color *color)
{
    uint32_t s0, t0, s1, t1;
    if (!color || !tmem_resolve_compiled_axis_fixed5(sample, s_fixed, true, &s0) ||
        !tmem_resolve_compiled_axis_fixed5(sample, s_fixed + 32, true, &s1) ||
        !tmem_resolve_compiled_axis_fixed5(sample, t_fixed, false, &t0) ||
        !tmem_resolve_compiled_axis_fixed5(sample, t_fixed + 32, false, &t1))
        return false;
    const rdp_tile *tile = &sample->tile;
    const int32_t shifted_s = shift_tile_coord_fixed5(s_fixed, tile->shift_s);
    const int32_t shifted_t = shift_tile_coord_fixed5(t_fixed, tile->shift_t);
    const uint32_t frac_s = (uint32_t)(shifted_s - ((int32_t)tile->sl << 3)) & 31u;
    const uint32_t frac_t = (uint32_t)(shifted_t - ((int32_t)tile->tl << 3)) & 31u;
    rdp_color c00, c10, c01, c11;
    if (sample->mid_texel && frac_s == 16u && frac_t == 16u) {
        if (tmem_fetch_rgba16_local(tmem, sample, s0, t0, &c00) &&
            tmem_fetch_rgba16_local(tmem, sample, s1, t0, &c10) &&
            tmem_fetch_rgba16_local(tmem, sample, s0, t1, &c01) &&
            tmem_fetch_rgba16_local(tmem, sample, s1, t1, &c11)) {
            *color = (rdp_color){
                (uint8_t)(((uint32_t)c00.r + c10.r + c01.r + c11.r + 2u) >> 2),
                (uint8_t)(((uint32_t)c00.g + c10.g + c01.g + c11.g + 2u) >> 2),
                (uint8_t)(((uint32_t)c00.b + c10.b + c01.b + c11.b + 2u) >> 2),
                (uint8_t)(((uint32_t)c00.a + c10.a + c01.a + c11.a + 2u) >> 2)
            };
            return true;
        }
    } else if (frac_s + frac_t >= 32u) {
        if (tmem_fetch_rgba16_local(tmem, sample, s1, t1, &c11) &&
            tmem_fetch_rgba16_local(tmem, sample, s1, t0, &c10) &&
            tmem_fetch_rgba16_local(tmem, sample, s0, t1, &c01)) {
            *color = bilerp_3tap_color(c11, c10, c01, 32u - frac_t, 32u - frac_s);
            return true;
        }
    } else {
        if (tmem_fetch_rgba16_local(tmem, sample, s0, t0, &c00) &&
            tmem_fetch_rgba16_local(tmem, sample, s1, t0, &c10) &&
            tmem_fetch_rgba16_local(tmem, sample, s0, t1, &c01)) {
            *color = bilerp_3tap_color(c00, c10, c01, frac_s, frac_t);
            return true;
        }
    }
    return tmem_fetch_rgba16_local(tmem, sample, s0, t0, color);
}

/*
 * Batched RGBA16 bilinear sampler.
 *
 * Semantically identical to calling tmem_sample_rgba16_bilerp_fixed5 once per
 * lane, but the per-sample invariants (tile shifts, origins, clamp/mirror/mask
 * axis parameters, TMEM base and stride) are hoisted into address-free locals
 * before the lane loop. In the scalar per-texel form these fields are re-read
 * from the sample struct on every call; because the fragment block that the
 * caller writes to is a uint16_t array, strict-aliasing forces the compiler to
 * reload the uint16_t sample fields after every store. Loading them into locals
 * here breaks that chain, and inlining the texel fetch and the 3-tap filter
 * lets the compiler fuse fetch + bilerp arithmetic (otherwise separate
 * out-of-line hot functions) into one straight-line body.
 *
 * Writes sampled colours into out[lane] for every lane set in `lanes`, and
 * returns the mask of lanes that sampled successfully. Lanes that fail (out of
 * bounds under a non-repeating axis, or a TMEM address past the end) are left
 * clear in the returned mask, matching a false return from the scalar path.
 */
static inline uint16_t tmem_sample_rgba16_bilerp_block(
    const tmem_state *tmem,
    const rdp_texture_sample_state *sample,
    const int32_t *s_fixed,
    const int32_t *t_fixed,
    uint16_t lanes,
    uint32_t count,
    rdp_color *out)
{
    if (!sample->width || !sample->height || !sample->stride) {
        return 0;
    }

    const rdp_tile *tile = &sample->tile;
    /* Address-free locals: not reloaded across the block stores. */
    const uint8_t shift_s = tile->shift_s;
    const uint8_t shift_t = tile->shift_t;
    const int32_t origin_s = (int32_t)tile->sl << 3;
    const int32_t origin_t = (int32_t)tile->tl << 3;
    const int32_t hi_s = sample->bounds.sh >= sample->bounds.sl
        ? (int32_t)(sample->bounds.sh - sample->bounds.sl) : 0;
    const int32_t hi_t = sample->bounds.th >= sample->bounds.tl
        ? (int32_t)(sample->bounds.th - sample->bounds.tl) : 0;
    const uint32_t extent_s = sample->width;
    const uint32_t extent_t = sample->height;
    const uint8_t mask_s = tile->mask_s;
    const uint8_t mask_t = tile->mask_t;
    const bool clamp_s = tile->clamp_s != 0 || tile->mask_s == 0;
    const bool clamp_t = tile->clamp_t != 0 || tile->mask_t == 0;
    const bool mirror_s = tile->mirror_s != 0;
    const bool mirror_t = tile->mirror_t != 0;
    const uint32_t tmem_base = tile->tmem;
    const uint32_t stride = sample->stride;
    const bool mid_texel = sample->mid_texel;

    uint16_t ok_mask = 0;
    for (uint32_t lane = 0; lane < count; lane++) {
        const uint16_t bit = (uint16_t)(1u << lane);
        if (!(lanes & bit)) continue;

        const int32_t shifted_s = shift_tile_coord_fixed5(s_fixed[lane], shift_s);
        const int32_t shifted_t = shift_tile_coord_fixed5(t_fixed[lane], shift_t);
        const int32_t shifted_s1 = shift_tile_coord_fixed5(s_fixed[lane] + 32, shift_s);
        const int32_t shifted_t1 = shift_tile_coord_fixed5(t_fixed[lane] + 32, shift_t);
        uint32_t s0, s1, t0, t1;
        if (!resolve_tile_axis(fixed5_floor_to_texel(shifted_s - origin_s), 0,
                               hi_s, extent_s, clamp_s, mirror_s, mask_s, &s0) ||
            !resolve_tile_axis(fixed5_floor_to_texel(shifted_s1 - origin_s), 0,
                               hi_s, extent_s, clamp_s, mirror_s, mask_s, &s1) ||
            !resolve_tile_axis(fixed5_floor_to_texel(shifted_t - origin_t), 0,
                               hi_t, extent_t, clamp_t, mirror_t, mask_t, &t0) ||
            !resolve_tile_axis(fixed5_floor_to_texel(shifted_t1 - origin_t), 0,
                               hi_t, extent_t, clamp_t, mirror_t, mask_t, &t1)) {
            continue;
        }

        const uint32_t frac_s = (uint32_t)(shifted_s - origin_s) & 31u;
        const uint32_t frac_t = (uint32_t)(shifted_t - origin_t) & 31u;

        /* Inlined RGBA16 texel fetch: logical -> physical word byte -> decode. */
        #define TMEM_RGBA16_TAP(dst, ls, lt)                                       \
            do {                                                                   \
                const uint32_t logical = tmem_base + (lt) * stride + (ls) * 2u;    \
                const uint32_t byte = tmem_physical_word_byte(                     \
                    logical ^ (((lt) & 1u) ? 4u : 0u));                            \
                if (byte + 1u >= SR_TMEM_SIZE) goto lane_fallback;                 \
                const uint16_t texel = ((uint16_t)tmem->bytes[byte] << 8) |        \
                                       (uint16_t)tmem->bytes[byte + 1u];           \
                (dst) = tmem_rgba16_decode_table[texel];                          \
            } while (0)

        rdp_color result;
        if (mid_texel && frac_s == 16u && frac_t == 16u) {
            rdp_color c00, c10, c01, c11;
            TMEM_RGBA16_TAP(c00, s0, t0);
            TMEM_RGBA16_TAP(c10, s1, t0);
            TMEM_RGBA16_TAP(c01, s0, t1);
            TMEM_RGBA16_TAP(c11, s1, t1);
            result = (rdp_color){
                (uint8_t)(((uint32_t)c00.r + c10.r + c01.r + c11.r + 2u) >> 2),
                (uint8_t)(((uint32_t)c00.g + c10.g + c01.g + c11.g + 2u) >> 2),
                (uint8_t)(((uint32_t)c00.b + c10.b + c01.b + c11.b + 2u) >> 2),
                (uint8_t)(((uint32_t)c00.a + c10.a + c01.a + c11.a + 2u) >> 2)
            };
        } else if (frac_s + frac_t >= 32u) {
            rdp_color c11, c10, c01;
            TMEM_RGBA16_TAP(c11, s1, t1);
            TMEM_RGBA16_TAP(c10, s1, t0);
            TMEM_RGBA16_TAP(c01, s0, t1);
            result = bilerp_3tap_color(c11, c10, c01, 32u - frac_t, 32u - frac_s);
        } else {
            rdp_color c00, c10, c01;
            TMEM_RGBA16_TAP(c00, s0, t0);
            TMEM_RGBA16_TAP(c10, s1, t0);
            TMEM_RGBA16_TAP(c01, s0, t1);
            result = bilerp_3tap_color(c00, c10, c01, frac_s, frac_t);
        }
        out[lane] = result;
        ok_mask |= bit;
        continue;

    lane_fallback:
        /* A tap ran off the end of TMEM: fall back to the base texel exactly as
         * the scalar path does at its tail. If even that address is invalid the
         * lane fails. */
        {
            const uint32_t logical = tmem_base + t0 * stride + s0 * 2u;
            const uint32_t byte = tmem_physical_word_byte(
                logical ^ ((t0 & 1u) ? 4u : 0u));
            if (byte + 1u >= SR_TMEM_SIZE) continue;
            const uint16_t texel = ((uint16_t)tmem->bytes[byte] << 8) |
                                   (uint16_t)tmem->bytes[byte + 1u];
            out[lane] = tmem_rgba16_decode_table[texel];
            ok_mask |= bit;
        }
        #undef TMEM_RGBA16_TAP
    }
    return ok_mask;
}

/*
 * Decodes one compact (4bpp/8bpp) texel from a resolved TMEM byte address,
 * matching the decode half of tmem_fetch_compact_local. Returns false only for
 * a CI8 palette entry that lands past the end of TMEM.
 */
static inline bool tmem_compact_decode_texel(const tmem_state *tmem,
                                             uint32_t byte, uint8_t subtexel,
                                             rdp_sampler_class sampler_class,
                                             bool tlut_ia, rdp_color *color)
{
    if (sampler_class == RDP_SAMPLER_I4_BILERP) {
        const uint8_t packed = tmem->bytes[byte];
        const uint8_t intensity = expand_4_to_8(
            subtexel ? (packed & 0xfu) : (packed >> 4));
        *color = (rdp_color){ intensity, intensity, intensity, intensity };
        return true;
    }
    if (sampler_class == RDP_SAMPLER_I8_BILERP) {
        const uint8_t intensity = tmem->bytes[byte];
        *color = (rdp_color){ intensity, intensity, intensity, intensity };
        return true;
    }
    if (sampler_class == RDP_SAMPLER_IA8_BILERP) {
        const uint8_t texel = tmem->bytes[byte];
        const uint8_t intensity = expand_4_to_8(texel >> 4);
        const uint8_t alpha = expand_4_to_8(texel);
        *color = (rdp_color){ intensity, intensity, intensity, alpha };
        return true;
    }
    const uint32_t palette_addr = 0x800u + (uint32_t)tmem->bytes[byte] * 8u;
    if (palette_addr + 1u >= SR_TMEM_SIZE) return false;
    const uint16_t entry = ((uint16_t)tmem->bytes[palette_addr] << 8) |
                           (uint16_t)tmem->bytes[palette_addr + 1u];
    if (tlut_ia) {
        const uint8_t intensity = (uint8_t)(entry >> 8);
        *color = (rdp_color){ intensity, intensity, intensity, (uint8_t)entry };
    } else {
        *color = tmem_rgba16_decode_table[entry];
    }
    return true;
}

/*
 * Batched compact (I4 / I8 / IA8 / CI8-TLUT) bilinear sampler. Same
 * invariant-hoisting rationale as tmem_sample_rgba16_bilerp_block; only the
 * texel fetch differs (4bpp/8bpp address resolution plus format-specific
 * decode inlined from tmem_fetch_compact_local). Semantically identical to a
 * per-lane tmem_sample_compact_bilerp_fixed5.
 */
static inline uint16_t tmem_sample_compact_bilerp_block(
    const tmem_state *tmem,
    const rdp_texture_sample_state *sample,
    const int32_t *s_fixed,
    const int32_t *t_fixed,
    uint16_t lanes,
    uint32_t count,
    rdp_color *out)
{
    if (!sample->width || !sample->height || !sample->stride) {
        return 0;
    }

    const rdp_tile *tile = &sample->tile;
    const uint8_t shift_s = tile->shift_s;
    const uint8_t shift_t = tile->shift_t;
    const int32_t origin_s = (int32_t)tile->sl << 3;
    const int32_t origin_t = (int32_t)tile->tl << 3;
    const int32_t hi_s = sample->bounds.sh >= sample->bounds.sl
        ? (int32_t)(sample->bounds.sh - sample->bounds.sl) : 0;
    const int32_t hi_t = sample->bounds.th >= sample->bounds.tl
        ? (int32_t)(sample->bounds.th - sample->bounds.tl) : 0;
    const uint32_t extent_s = sample->width;
    const uint32_t extent_t = sample->height;
    const uint8_t mask_s = tile->mask_s;
    const uint8_t mask_t = tile->mask_t;
    const bool clamp_s = tile->clamp_s != 0 || tile->mask_s == 0;
    const bool clamp_t = tile->clamp_t != 0 || tile->mask_t == 0;
    const bool mirror_s = tile->mirror_s != 0;
    const bool mirror_t = tile->mirror_t != 0;
    const uint32_t tmem_base = tile->tmem;
    const uint32_t stride = sample->stride;
    const bool mid_texel = sample->mid_texel;
    const rdp_sampler_class sampler_class = sample->sampler_class;
    const bool tlut_ia = sample->tlut_ia;
    const bool is_4bpp = tile->size == RDP_SIZE_4BPP;
    const bool valid_size = tile->size == RDP_SIZE_4BPP || tile->size == RDP_SIZE_8BPP;

    uint16_t ok_mask = 0;
    for (uint32_t lane = 0; lane < count; lane++) {
        const uint16_t bit = (uint16_t)(1u << lane);
        if (!(lanes & bit)) continue;

        const int32_t shifted_s = shift_tile_coord_fixed5(s_fixed[lane], shift_s);
        const int32_t shifted_t = shift_tile_coord_fixed5(t_fixed[lane], shift_t);
        const int32_t shifted_s1 = shift_tile_coord_fixed5(s_fixed[lane] + 32, shift_s);
        const int32_t shifted_t1 = shift_tile_coord_fixed5(t_fixed[lane] + 32, shift_t);
        uint32_t s0, s1, t0, t1;
        if (!resolve_tile_axis(fixed5_floor_to_texel(shifted_s - origin_s), 0,
                               hi_s, extent_s, clamp_s, mirror_s, mask_s, &s0) ||
            !resolve_tile_axis(fixed5_floor_to_texel(shifted_s1 - origin_s), 0,
                               hi_s, extent_s, clamp_s, mirror_s, mask_s, &s1) ||
            !resolve_tile_axis(fixed5_floor_to_texel(shifted_t - origin_t), 0,
                               hi_t, extent_t, clamp_t, mirror_t, mask_t, &t0) ||
            !resolve_tile_axis(fixed5_floor_to_texel(shifted_t1 - origin_t), 0,
                               hi_t, extent_t, clamp_t, mirror_t, mask_t, &t1)) {
            continue;
        }

        const uint32_t frac_s = (uint32_t)(shifted_s - origin_s) & 31u;
        const uint32_t frac_t = (uint32_t)(shifted_t - origin_t) & 31u;

        /* Inlined compact texel fetch: 4bpp/8bpp address -> format decode. */
        #define TMEM_COMPACT_TAP(dst, ls, lt)                                     \
            do {                                                                  \
                if (!valid_size) goto lane_fallback;                              \
                const uint32_t rxor = ((lt) & 1u) ? 4u : 0u;                      \
                uint32_t byte;                                                    \
                uint8_t subtexel;                                                 \
                if (is_4bpp) {                                                    \
                    byte = (tmem_base + (lt) * stride + (((ls) >> 1) ^ rxor)) ^ 2u;\
                    subtexel = (uint8_t)((ls) & 1u);                              \
                } else {                                                          \
                    byte = (tmem_base + (lt) * stride + ((ls) ^ rxor)) ^ 2u;      \
                    subtexel = 0u;                                                \
                }                                                                 \
                if (byte >= SR_TMEM_SIZE) goto lane_fallback;                     \
                if (!tmem_compact_decode_texel(tmem, byte, subtexel,             \
                        sampler_class, tlut_ia, &(dst))) goto lane_fallback;      \
            } while (0)

        rdp_color result;
        if (mid_texel && frac_s == 16u && frac_t == 16u) {
            rdp_color c00, c10, c01, c11;
            TMEM_COMPACT_TAP(c00, s0, t0);
            TMEM_COMPACT_TAP(c10, s1, t0);
            TMEM_COMPACT_TAP(c01, s0, t1);
            TMEM_COMPACT_TAP(c11, s1, t1);
            result = (rdp_color){
                (uint8_t)(((uint32_t)c00.r + c10.r + c01.r + c11.r + 2u) >> 2),
                (uint8_t)(((uint32_t)c00.g + c10.g + c01.g + c11.g + 2u) >> 2),
                (uint8_t)(((uint32_t)c00.b + c10.b + c01.b + c11.b + 2u) >> 2),
                (uint8_t)(((uint32_t)c00.a + c10.a + c01.a + c11.a + 2u) >> 2)
            };
        } else if (frac_s + frac_t >= 32u) {
            rdp_color c11, c10, c01;
            TMEM_COMPACT_TAP(c11, s1, t1);
            TMEM_COMPACT_TAP(c10, s1, t0);
            TMEM_COMPACT_TAP(c01, s0, t1);
            result = bilerp_3tap_color(c11, c10, c01, 32u - frac_t, 32u - frac_s);
        } else {
            rdp_color c00, c10, c01;
            TMEM_COMPACT_TAP(c00, s0, t0);
            TMEM_COMPACT_TAP(c10, s1, t0);
            TMEM_COMPACT_TAP(c01, s0, t1);
            result = bilerp_3tap_color(c00, c10, c01, frac_s, frac_t);
        }
        out[lane] = result;
        ok_mask |= bit;
        continue;

    lane_fallback:
        /* Tail fallback to the base texel, matching the scalar path's return. */
        {
            if (!valid_size) continue;
            const uint32_t rxor = (t0 & 1u) ? 4u : 0u;
            uint32_t byte;
            uint8_t subtexel;
            if (is_4bpp) {
                byte = (tmem_base + t0 * stride + ((s0 >> 1) ^ rxor)) ^ 2u;
                subtexel = (uint8_t)(s0 & 1u);
            } else {
                byte = (tmem_base + t0 * stride + (s0 ^ rxor)) ^ 2u;
                subtexel = 0u;
            }
            if (byte >= SR_TMEM_SIZE) continue;
            rdp_color result;
            if (!tmem_compact_decode_texel(tmem, byte, subtexel, sampler_class,
                                           tlut_ia, &result)) continue;
            out[lane] = result;
            ok_mask |= bit;
        }
        #undef TMEM_COMPACT_TAP
    }
    return ok_mask;
}

static inline bool tmem_sample_bilerp_compiled_fixed5(const tmem_state *tmem,
                                                       const rdp_texture_sample_state *sample,
                                                       int32_t s_fixed, int32_t t_fixed,
                                                       rdp_color *color)
{
    uint32_t local_s, local_t;
    if (!color || !tmem_resolve_compiled_coord_fixed5(sample, s_fixed, t_fixed,
                                                       &local_s, &local_t)) return false;

    {
        uint32_t local_s1;
        uint32_t local_t1;
        rdp_color c00, c10, c01, c11;
        const rdp_tile *tile = &sample->tile;
        const int32_t shifted_s = shift_tile_coord_fixed5(s_fixed, tile->shift_s);
        const int32_t shifted_t = shift_tile_coord_fixed5(t_fixed, tile->shift_t);
        const int32_t rel_s = shifted_s - ((int32_t)tile->sl << 3);
        const int32_t rel_t = shifted_t - ((int32_t)tile->tl << 3);
        const uint32_t frac_s = (uint32_t)rel_s & 31u;
        const uint32_t frac_t = (uint32_t)rel_t & 31u;

        if (tmem_resolve_compiled_axis_fixed5(sample, s_fixed + 32, true, &local_s1) &&
            tmem_resolve_compiled_axis_fixed5(sample, t_fixed + 32, false, &local_t1) &&
            tmem_fetch_color_local(tmem, sample, local_s, local_t, &c00) &&
            tmem_fetch_color_local(tmem, sample, local_s1, local_t, &c10) &&
            tmem_fetch_color_local(tmem, sample, local_s, local_t1, &c01) &&
            tmem_fetch_color_local(tmem, sample, local_s1, local_t1, &c11)) {
            if (sample->mid_texel && frac_s == 16u && frac_t == 16u) {
                *color = (rdp_color){
                    (uint8_t)(((uint32_t)c00.r + c10.r + c01.r + c11.r + 2u) >> 2),
                    (uint8_t)(((uint32_t)c00.g + c10.g + c01.g + c11.g + 2u) >> 2),
                    (uint8_t)(((uint32_t)c00.b + c10.b + c01.b + c11.b + 2u) >> 2),
                    (uint8_t)(((uint32_t)c00.a + c10.a + c01.a + c11.a + 2u) >> 2)
                };
                return true;
            }

            if (frac_s + frac_t >= 32u) {
                *color = bilerp_3tap_color(c11, c10, c01, 32u - frac_t, 32u - frac_s);
            } else {
                *color = bilerp_3tap_color(c00, c10, c01, frac_s, frac_t);
            }
            return true;
        }
    }

    return tmem_fetch_color_local(tmem, sample, local_s, local_t, color);
}

static inline bool tmem_sample_color_fixed5(const tmem_state *tmem, const rdp_texture_sample_state *sample, int32_t s_fixed, int32_t t_fixed, rdp_color *color)
{
    if (!color || !sample || sample->tile_index >= 8) return false;
    if (sample->width && sample->height && sample->stride) {
        if (sample->sampler_class == RDP_SAMPLER_RGBA16_BILERP)
            return tmem_sample_rgba16_bilerp_fixed5(tmem, sample, s_fixed, t_fixed, color);
        if (sample->sampler_class == RDP_SAMPLER_RGBA16_POINT)
            return tmem_sample_rgba16_point_fixed5(tmem, sample, s_fixed, t_fixed, color);
        if (sample->sampler_class == RDP_SAMPLER_I4_BILERP ||
            sample->sampler_class == RDP_SAMPLER_CI8_TLUT_BILERP ||
            sample->sampler_class == RDP_SAMPLER_I8_BILERP ||
            sample->sampler_class == RDP_SAMPLER_IA8_BILERP)
            return tmem_sample_compact_bilerp_fixed5(tmem, sample, s_fixed, t_fixed, color);
        return sample->bilerp && sample->sample_quad
            ? tmem_sample_bilerp_compiled_fixed5(tmem, sample, s_fixed, t_fixed, color)
            : tmem_sample_point_compiled_fixed5(tmem, sample, s_fixed, t_fixed, color);
    }
    uint32_t local_s, local_t;
    if (!tmem_resolve_texel_coord_fixed5(tmem, sample, s_fixed, t_fixed, &local_s, &local_t)) return false;
    return tmem_fetch_color_local(tmem, sample, local_s, local_t, color);
}

#endif
