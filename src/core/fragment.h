#ifndef FRAGMENT_H
#define FRAGMENT_H

#include "pipeline.h"

sr_result fragment_finish(sr_memory *memory,
                          const rdp_primitive_state *primitive,
                          uint32_t x, uint32_t y,
                          rdp_color pixel,
                          uint8_t shade_alpha,
                          bool *accepted);

#endif
