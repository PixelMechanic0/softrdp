#ifndef COMBINER_H
#define COMBINER_H

#include "rdp_state.h"



typedef struct rdp_combiner_inputs {
    rdp_color shade;
    rdp_color texel0;
    rdp_color texel1;
    rdp_color primitive;
    rdp_color environment;
    uint8_t lod_fraction;
    uint8_t primitive_lod_fraction;
    uint16_t k4;
    uint16_t k5;
} rdp_combiner_inputs;

#define RDP_PACKET_LANES 16u

typedef struct rdp_combiner_packet {
    uint32_t count;
    uint16_t shade[4][RDP_PACKET_LANES];
    uint16_t texel0[4][RDP_PACKET_LANES];
    uint16_t texel1[4][RDP_PACKET_LANES];
    uint16_t output[4][RDP_PACKET_LANES];
    uint16_t lod_fraction[RDP_PACKET_LANES];
} rdp_combiner_packet;

void rdp_combiner_decode(rdp_combiner_program *program, uint32_t w0, uint32_t w1);
void rdp_combiner_make_passthrough(rdp_combiner_program *program,
                                   rdp_combiner_source rgb,
                                   rdp_combiner_source alpha);
rdp_color rdp_combiner_evaluate(const rdp_combiner_program *program,
                                rdp_cycle_type cycle_type,
                                const rdp_combiner_inputs *inputs);
void rdp_combiner_evaluate_packet(const rdp_color_pipeline_state *state,
                                  rdp_combiner_packet *packet);

#endif
