#ifndef COMBINER_H
#define COMBINER_H

#include "rdp_state.h"



typedef struct rdp_combiner_inputs {
    rdp_color shade;
    rdp_color texel0;
    rdp_color texel1;
    rdp_color primitive;
    rdp_color environment;
    uint16_t lod_fraction;
    uint8_t primitive_lod_fraction;
    uint16_t k4;
    uint16_t k5;
} rdp_combiner_inputs;

#define RDP_PACKET_LANES 16u

typedef struct rdp_fragment_block {
    uint32_t count;
    uint16_t active_mask;
    uint16_t accepted_mask;
    uint16_t fallback_mask;
    uint32_t x[RDP_PACKET_LANES];
    uint32_t y;
    uint32_t color_address[RDP_PACKET_LANES];
    int64_t depth_fixed[RDP_PACKET_LANES];
    uint32_t depth_address[RDP_PACKET_LANES];
    uint16_t depth_value[RDP_PACKET_LANES];
    uint16_t depth_update_mask;
    int32_t s[RDP_PACKET_LANES];
    int32_t t[RDP_PACKET_LANES];
    int32_t w[RDP_PACKET_LANES];
    int32_t sample_s[RDP_PACKET_LANES];
    int32_t sample_t[RDP_PACKET_LANES];
    uint16_t shade[4][RDP_PACKET_LANES];
    uint16_t texel0[4][RDP_PACKET_LANES];
    uint16_t texel1[4][RDP_PACKET_LANES];
    uint16_t color[4][RDP_PACKET_LANES];
    uint16_t lod_fraction[RDP_PACKET_LANES];
    uint16_t alpha[RDP_PACKET_LANES];
    uint8_t coverage[RDP_PACKET_LANES];
} rdp_fragment_block;

void rdp_combiner_decode(rdp_combiner_program *program, uint32_t w0, uint32_t w1);
void rdp_combiner_make_passthrough(rdp_combiner_program *program,
                                   rdp_combiner_source rgb,
                                   rdp_combiner_source alpha);
rdp_color rdp_combiner_evaluate(const rdp_combiner_program *program,
                                rdp_cycle_type cycle_type,
                                const rdp_combiner_inputs *inputs);
void rdp_combiner_evaluate_packet(const rdp_color_pipeline_state *state,
                                  rdp_fragment_block *packet);

#endif
