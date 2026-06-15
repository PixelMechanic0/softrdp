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

pipeline_outputs pipeline_shade_pixel(const rdp_state *state, const pipeline_inputs *inputs)
{
    pipeline_outputs out;

    if (!inputs) {
        out.color = (rdp_color){0, 0, 0, 255};
    } else if (state && state->simple_combiner == RDP_SIMPLE_COMBINER_PRIMITIVE) {
        out.color = inputs->primitive;
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
