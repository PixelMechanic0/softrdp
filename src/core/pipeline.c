#include "pipeline.h"

static uint8_t expand_5_to_8(uint32_t value)
{
    value &= 0x1fu;
    return (uint8_t)((value << 3) | (value >> 2));
}

static uint8_t shrink_8_to_5(uint8_t value)
{
    return (uint8_t)(value >> 3);
}

static uint8_t multiply_u8(uint8_t a, uint8_t b)
{
    return (uint8_t)(((uint32_t)a * (uint32_t)b + 127u) / 255u);
}

static rdp_color modulate_color(rdp_color texel, rdp_color shade)
{
    return (rdp_color){
        multiply_u8(texel.r, shade.r),
        multiply_u8(texel.g, shade.g),
        multiply_u8(texel.b, shade.b),
        multiply_u8(texel.a, shade.a)
    };
}

pipeline_outputs pipeline_shade_pixel(const rdp_state *state, const pipeline_inputs *inputs)
{
    pipeline_outputs out;

    if (!inputs) {
        out.color = (rdp_color){0, 0, 0, 255};
    } else if (state && state->simple_combiner == RDP_SIMPLE_COMBINER_PRIMITIVE) {
        out.color = inputs->primitive;
    } else if (state && state->simple_combiner == RDP_SIMPLE_COMBINER_TEXEL0_SHADE) {
        out.color = modulate_color(inputs->texel0, inputs->shade);
    } else {
        out.color = inputs->texel0;
    }

    out.coverage = 7;
    return out;
}

rdp_color pipeline_rgba5551_to_color(uint16_t value)
{
    rdp_color color;

    color.r = expand_5_to_8(value >> 11);
    color.g = expand_5_to_8(value >> 6);
    color.b = expand_5_to_8(value >> 1);
    color.a = (value & 1u) ? 255u : 0u;
    return color;
}

uint16_t pipeline_color_to_rgba5551(rdp_color color)
{
    return (uint16_t)(((uint16_t)shrink_8_to_5(color.r) << 11) |
                      ((uint16_t)shrink_8_to_5(color.g) << 6) |
                      ((uint16_t)shrink_8_to_5(color.b) << 1) |
                      (color.a >= 128 ? 1u : 0u));
}

uint32_t pipeline_color_to_rgba8888(rdp_color color)
{
    return ((uint32_t)color.r << 24) |
           ((uint32_t)color.g << 16) |
           ((uint32_t)color.b << 8) |
           (uint32_t)color.a;
}
