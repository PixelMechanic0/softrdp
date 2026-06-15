#ifndef PIPELINE_H
#define PIPELINE_H

#include "rdp_state.h"

typedef struct pipeline_inputs {
    rdp_color shade;
    rdp_color texel0;
    rdp_color texel1;
    rdp_color primitive;
} pipeline_inputs;

typedef struct pipeline_outputs {
    rdp_color color;
    uint8_t coverage;
} pipeline_outputs;

pipeline_outputs pipeline_shade_pixel(const rdp_state *state, const pipeline_inputs *inputs);
rdp_color pipeline_rgba5551_to_color(uint16_t value);
uint16_t pipeline_color_to_rgba5551(rdp_color color);
uint32_t pipeline_color_to_rgba8888(rdp_color color);

#endif
