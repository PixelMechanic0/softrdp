#ifndef PIPELINE_H
#define PIPELINE_H

#include "rdp_commands.h"

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

sr_result pipeline_process_triangle_pixel(sr_memory *memory,
                                          tmem_state *tmem,
                                          const rdp_state *state,
                                          const raster_decoded_triangle *decoded,
                                          int x, int y,
                                          bool fill_mode,
                                          const rdp_tile_bounds *bounds);

sr_result pipeline_process_triangle_span(sr_memory *memory,
                                         tmem_state *tmem,
                                         const rdp_state *state,
                                         const raster_decoded_triangle *decoded,
                                         int x0, int x1, int y,
                                         bool fill_mode,
                                         const rdp_tile_bounds *bounds);

sr_result pipeline_process_rect_pixel(sr_memory *memory,
                                      tmem_state *tmem,
                                      const rdp_state *state,
                                      uint32_t tile_index,
                                      uint32_t s, uint32_t t,
                                      uint32_t x, uint32_t y,
                                      const rdp_tile_bounds *bounds);

void pipeline_resolve_tile_bounds(const rdp_state *state, const tmem_state *tmem, uint32_t tile_index, rdp_tile_bounds *bounds);

#endif
