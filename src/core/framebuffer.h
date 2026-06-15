#ifndef FRAMEBUFFER_H
#define FRAMEBUFFER_H

#include "rdp_memory.h"
#include "rdp_state.h"

sr_result framebuffer_fill_rect(sr_memory *memory,
                                const rdp_state *state,
                                uint32_t x0,
                                uint32_t y0,
                                uint32_t x1,
                                uint32_t y1);
sr_result framebuffer_write_rgba5551(sr_memory *memory,
                                     const rdp_state *state,
                                     uint32_t x,
                                     uint32_t y,
                                     uint16_t texel);
sr_result framebuffer_write_color(sr_memory *memory,
                                  const rdp_state *state,
                                  uint32_t x,
                                  uint32_t y,
                                  rdp_color color);
sr_result framebuffer_write_fill_pixel(sr_memory *memory,
                                       const rdp_state *state,
                                       uint32_t x,
                                       uint32_t y);

#endif
