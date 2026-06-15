#include "rdp_state.h"

#include <string.h>

void rdp_state_init(rdp_state *state)
{
    memset(state, 0, sizeof(*state));

    state->color_image.format = RDP_FORMAT_RGBA;
    state->color_image.size = RDP_SIZE_16BPP;
    state->color_image.width = 320;

    state->texture_image.format = RDP_FORMAT_RGBA;
    state->texture_image.size = RDP_SIZE_16BPP;
    state->texture_image.width = 1;

    state->scissor_x1 = 640u << 2;
    state->scissor_y1 = 480u << 2;
    state->other_modes.cycle_type = RDP_CYCLE_1;
    state->other_modes.bilerp0 = true;
    state->other_modes.bilerp1 = true;
    state->simple_combiner = RDP_SIMPLE_COMBINER_TEXEL0;
}
