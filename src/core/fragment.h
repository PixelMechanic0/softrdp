#ifndef FRAGMENT_H
#define FRAGMENT_H

#include "pipeline.h"

typedef struct rdp_fragment_packet {
    uint32_t count;
    uint16_t active_mask;
    uint16_t accepted_mask;
    uint32_t x[RDP_PACKET_LANES];
    uint32_t y[RDP_PACKET_LANES];
    uint16_t color[4][RDP_PACKET_LANES];
    uint16_t shade_alpha[RDP_PACKET_LANES];
} rdp_fragment_packet;

sr_result fragment_finish_packet(sr_memory *memory,
                                 const rdp_primitive_state *primitive,
                                 rdp_fragment_packet *packet);

#endif
