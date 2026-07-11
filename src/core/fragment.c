#include "fragment.h"

#include "blender.h"
#include "framebuffer.h"

sr_result fragment_finish(sr_memory *memory,
                          const rdp_primitive_state *primitive,
                          uint32_t x, uint32_t y,
                          rdp_color pixel,
                          uint8_t shade_alpha,
                          bool *accepted)
{
    if (accepted) *accepted = false;
    const rdp_fragment_state *state = &primitive->fragment;
    rdp_fragment fragment = {
        .color = pixel, .alpha = pixel.a, .coverage = 8u,
        .shade_alpha = shade_alpha
    };
    if (state->cvg_times_alpha) {
        fragment.alpha = (uint16_t)(((uint32_t)fragment.alpha * fragment.coverage + 4u) >> 3);
        fragment.coverage = (uint8_t)((fragment.alpha >> 5) & 0xfu);
    }
    if (state->alpha_cvg_select && !state->cvg_times_alpha)
        fragment.alpha = (uint16_t)fragment.coverage << 5;
    if (state->blend.alpha_compare && fragment.alpha < state->blend.blend_color.a)
        return SR_OK;

    rdp_memory_pixel memory_pixel;
    const sr_result read = framebuffer_read_memory_pixel(memory, &primitive->framebuffer,
                                                         x, y, state->blend.image_read,
                                                         &memory_pixel);
    if (read != SR_OK) return read;
    const bool overflow = ((fragment.coverage + memory_pixel.coverage) & 8u) != 0u;
    const bool blend = state->blend.force_blend || (state->antialias && !overflow);
    fragment.color = rdp_blender_evaluate(&state->blend, fragment.color,
                                          fragment.alpha, memory_pixel.color,
                                          fragment.shade_alpha, blend);

    uint32_t final_coverage;
    switch (state->coverage_dest & 3u) {
    case 0:
        final_coverage = blend ? fragment.coverage + memory_pixel.coverage
                               : fragment.coverage - 1u;
        if (final_coverage > 7u) final_coverage = 7u;
        break;
    case 1: final_coverage = (fragment.coverage + memory_pixel.coverage) & 7u; break;
    case 2: final_coverage = 7u; break;
    default: final_coverage = memory_pixel.coverage; break;
    }
    fragment.color.a = (uint8_t)((final_coverage << 5) | 0x1fu);
    const sr_result result = framebuffer_write_color(memory, &primitive->framebuffer,
                                                     x, y, fragment.color);
    if (result == SR_OK && accepted) *accepted = true;
    return result;
}
