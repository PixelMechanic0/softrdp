#ifndef BLENDER_H
#define BLENDER_H

#include "rdp_state.h"

void rdp_blender_decode(rdp_blender_program *program, uint32_t w1);
rdp_color rdp_blender_evaluate(const rdp_blend_state *state,
                               rdp_color pixel,
                               uint16_t pixel_alpha,
                               rdp_color memory,
                               uint8_t shade_alpha,
                               bool blend_enable);
rdp_color rdp_blender_evaluate_coverage(const rdp_blend_state *state,
                                        rdp_color pixel,
                                        uint16_t pixel_alpha,
                                        rdp_color memory,
                                        uint8_t shade_alpha,
                                        bool blend_enable,
                                        bool color_on_coverage,
                                        bool coverage_wrap);

#endif
