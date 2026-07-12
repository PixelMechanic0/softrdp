#ifndef FRAGMENT_H
#define FRAGMENT_H

#include "pipeline.h"

sr_result fragment_finish_packet(sr_memory *memory,
                                 const rdp_primitive_state *primitive,
                                 rdp_fragment_block *packet);

#endif
