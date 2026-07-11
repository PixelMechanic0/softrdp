#ifndef TMEM_H
#define TMEM_H

#include "rdp_state.h"
#include "rdp_metrics.h"
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

void tmem_init(tmem_state *tmem);
sr_result tmem_load_tile(tmem_state *tmem, sr_memory *memory, const rdp_state *state, rdp_metrics *metrics, const rdp_command *cmd);

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
    return byte ^ 3u;
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
    address->byte = tmem_physical_word_byte(logical ^ ((local_t & 1u) ? 4u : 0u));
    address->byte2 = address->byte + 0x800u;
    address->subtexel = 0;
    address->bytes = 4;
    return address->byte + 1u < SR_TMEM_SIZE && address->byte2 + 1u < SR_TMEM_SIZE;
}

static inline bool tmem_tile_extent_from_descriptor(const rdp_tile *tile, uint32_t *width, uint32_t *height)
{
    if (!tile || !width || !height || tile->sh < tile->sl || tile->th < tile->tl ||
        (tile->sh == tile->sl && tile->th == tile->tl)) {
        return false;
    }

    *width = (tile->sh >> 2) - (tile->sl >> 2) + 1u;
    *height = (tile->th >> 2) - (tile->tl >> 2) + 1u;
    return *width != 0 && *height != 0;
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
    if (tmem_tile_extent_from_descriptor(tile, width, height)) {
        *stride = tile->line;
        if (*stride == 0) {
            const uint32_t bytes_per_texel = tile->size == RDP_SIZE_32BPP ? 4u :
                                             tile->size == RDP_SIZE_16BPP ? 2u :
                                             tile->size == RDP_SIZE_8BPP ? 1u : 0u;
            if (bytes_per_texel == 0) {
                return false;
            }
            *stride = tmem_align_row_stride(*width * bytes_per_texel);
        }
    } else if (tmem->tile_width[tile_index] != 0 && tmem->tile_height[tile_index] != 0) {
        *width = tmem->tile_width[tile_index];
        *height = tmem->tile_height[tile_index];
        *stride = tmem->tile_stride[tile_index];
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

static inline uint8_t bilerp_3tap_u8(uint8_t base, uint8_t edge_s, uint8_t edge_t, uint32_t frac_s, uint32_t frac_t)
{
    int32_t value = ((int32_t)edge_s - (int32_t)base) * (int32_t)frac_s;
    value += ((int32_t)edge_t - (int32_t)base) * (int32_t)frac_t;
    value = (value + 0x10) >> 5;
    value += base;
    return clamp_i32_to_u8(value);
}

static inline rdp_color bilerp_3tap_color(rdp_color base, rdp_color edge_s, rdp_color edge_t, uint32_t frac_s, uint32_t frac_t)
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
    if (!tmem_resolve_texel_address(tmem, sample, local_s, local_t, &address)) {
        return false;
    }

    if (tile->format == RDP_FORMAT_CI && sample->tlut_enable) {
        uint32_t index;
        if (tile->size == RDP_SIZE_4BPP && address.bytes == 1) {
            const uint8_t packed = tmem->bytes[address.byte];
            index = ((uint32_t)tile->palette << 4) |
                    (address.subtexel ? (packed & 0xfu) : (packed >> 4));
        } else if (tile->size == RDP_SIZE_8BPP && address.bytes == 1) {
            index = tmem->bytes[address.byte];
        } else {
            return false;
        }

        const uint32_t palette_addr = 0x800u + index * 8u;
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

static inline bool tmem_sample_color_fixed5(const tmem_state *tmem, const rdp_texture_sample_state *sample, int32_t s_fixed, int32_t t_fixed, rdp_color *color)
{
    if (!color || !sample || sample->tile_index >= 8) {
        return false;
    }

    uint32_t local_s;
    uint32_t local_t;
    if (!tmem_resolve_texel_coord_fixed5(tmem, sample, s_fixed, t_fixed, &local_s, &local_t)) {
        return false;
    }

    if (sample->bilerp && sample->sample_quad) {
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

        if (tmem_resolve_texel_coord_fixed5(tmem, sample, s_fixed + 32, t_fixed, &local_s1, &local_t) &&
            tmem_resolve_texel_coord_fixed5(tmem, sample, s_fixed, t_fixed + 32, &local_s, &local_t1) &&
            tmem_resolve_texel_coord_fixed5(tmem, sample, s_fixed + 32, t_fixed + 32, &local_s1, &local_t1) &&
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

#endif
