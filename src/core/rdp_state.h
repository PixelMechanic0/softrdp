#ifndef RDP_STATE_H
#define RDP_STATE_H

#include "sr_defs.h"

typedef enum rdp_cycle_type {
    RDP_CYCLE_1 = 0,
    RDP_CYCLE_2 = 1,
    RDP_CYCLE_COPY = 2,
    RDP_CYCLE_FILL = 3
} rdp_cycle_type;

typedef enum rdp_texture_format {
    RDP_FORMAT_RGBA = 0,
    RDP_FORMAT_YUV = 1,
    RDP_FORMAT_CI = 2,
    RDP_FORMAT_IA = 3,
    RDP_FORMAT_I = 4
} rdp_texture_format;

typedef enum rdp_texture_size {
    RDP_SIZE_4BPP = 0,
    RDP_SIZE_8BPP = 1,
    RDP_SIZE_16BPP = 2,
    RDP_SIZE_32BPP = 3
} rdp_texture_size;

typedef struct rdp_color {
    uint8_t r;
    uint8_t g;
    uint8_t b;
    uint8_t a;
} rdp_color;

typedef struct rdp_image {
    uint32_t address;
    uint32_t width;
    rdp_texture_format format;
    rdp_texture_size size;
} rdp_image;

typedef struct rdp_tile {
    uint16_t sl;
    uint16_t tl;
    uint16_t sh;
    uint16_t th;
    uint16_t tmem;
    uint16_t line;
    uint8_t palette;
    uint8_t clamp_s;
    uint8_t clamp_t;
    uint8_t mirror_s;
    uint8_t mirror_t;
    uint8_t mask_s;
    uint8_t mask_t;
    uint8_t shift_s;
    uint8_t shift_t;
    rdp_texture_format format;
    rdp_texture_size size;
} rdp_tile;

typedef struct rdp_other_modes {
    rdp_cycle_type cycle_type;
    bool perspective;
    bool texture_lod;
    bool sharpen_lod;
    bool detail_lod;
    bool tlut_enable;
    bool tlut_ia;
    bool sample_quad;
    bool mid_texel;
    bool bilerp0;
    bool bilerp1;
    bool convert_one;
    bool force_blend;
    bool image_read;
    bool z_update;
    bool z_compare;
    bool antialias;
    bool alpha_compare;
    uint8_t coverage_dest;
    uint8_t z_mode;
    uint8_t rgb_dither;
    uint8_t alpha_dither;
} rdp_other_modes;

typedef enum rdp_simple_combiner {
    RDP_SIMPLE_COMBINER_TEXEL0 = 0,
    RDP_SIMPLE_COMBINER_PRIMITIVE = 1,
    RDP_SIMPLE_COMBINER_TEXEL0_SHADE = 2
} rdp_simple_combiner;

typedef struct rdp_tile_bounds {
    uint32_t sl;
    uint32_t tl;
    uint32_t sh;
    uint32_t th;
} rdp_tile_bounds;

typedef struct rdp_state {
    rdp_image color_image;
    rdp_image texture_image;
    uint32_t depth_image_address;
    uint32_t fill_color;
    rdp_color fog_color;
    rdp_color blend_color;
    rdp_color primitive_color;
    rdp_color environment_color;
    uint8_t primitive_min_lod;
    uint8_t primitive_lod_fraction;
    uint16_t primitive_depth;
    uint16_t primitive_delta_z;
    uint16_t scissor_x0;
    uint16_t scissor_y0;
    uint16_t scissor_x1;
    uint16_t scissor_y1;
    rdp_other_modes other_modes;
    rdp_simple_combiner simple_combiner;
    bool combiner_needs_texel0;
    bool combiner_needs_shade;
    rdp_tile tiles[8];
    uint64_t commands_seen;
    uint64_t draw_calls_seen;
} rdp_state;

static inline uint8_t expand_5_to_8(uint32_t value)
{
    value &= 0x1fu;
    return (uint8_t)((value << 3) | (value >> 2));
}

static inline uint8_t shrink_8_to_5(uint8_t value)
{
    return (uint8_t)(value >> 3);
}

static inline rdp_color pipeline_rgba5551_to_color(uint16_t value)
{
    return (rdp_color){ expand_5_to_8(value >> 11), expand_5_to_8(value >> 6), expand_5_to_8(value >> 1), (value & 1u) ? 255u : 0u };
}

static inline uint16_t pipeline_color_to_rgba5551(rdp_color color)
{
    return (uint16_t)(((uint16_t)shrink_8_to_5(color.r) << 11) |
                      ((uint16_t)shrink_8_to_5(color.g) << 6) |
                      ((uint16_t)shrink_8_to_5(color.b) << 1) |
                      (color.a >= 128 ? 1u : 0u));
}

static inline uint32_t pipeline_color_to_rgba8888(rdp_color color)
{
    return ((uint32_t)color.r << 24) | ((uint32_t)color.g << 16) | ((uint32_t)color.b << 8) | (uint32_t)color.a;
}

#endif
