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
    rdp_tile tiles[8];
    uint64_t commands_seen;
    uint64_t draw_calls_seen;
} rdp_state;

void rdp_state_init(rdp_state *state);

#endif
