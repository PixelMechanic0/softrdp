#include "fragment.h"

#include "blender.h"
#include "framebuffer.h"

sr_result fragment_finish_packet(sr_memory *memory,
                                 const rdp_primitive_state *primitive,
                                 rdp_fragment_block *packet)
{
    if (!memory || !primitive || !packet || packet->count > RDP_PACKET_LANES)
        return SR_ERROR_INVALID_ARGUMENT;

    const rdp_fragment_state *state = &primitive->fragment;
    uint16_t active = packet->active_mask;

    /* Alpha and coverage are pure packet arithmetic. Rejected lanes remain in
     * the packet and are removed from the shared active mask. */
    for (uint32_t lane = 0; lane < packet->count; lane++) {
        const uint16_t bit = (uint16_t)(1u << lane);
        packet->coverage[lane] = 8u;
        packet->alpha[lane] = packet->color[3][lane];
        if (!(active & bit)) continue;
        if (state->cvg_times_alpha) {
            packet->alpha[lane] = (uint16_t)(((uint32_t)packet->alpha[lane] * packet->coverage[lane] + 4u) >> 3);
            packet->coverage[lane] = (uint8_t)((packet->alpha[lane] >> 5) & 0xfu);
        }
        if (state->alpha_cvg_select && !state->cvg_times_alpha)
            packet->alpha[lane] = (uint16_t)packet->coverage[lane] << 5;
        if (state->blend.alpha_compare && packet->alpha[lane] < state->blend.blend_color.a) {
            active &= (uint16_t)~bit;
        }
    }

    packet->accepted_mask = active;
    /* The remaining scalar stages share one lane-local color value. This keeps
     * framebuffer read, blender, coverage destination, and commit in one pass
     * without materializing another packet representation. */
    for (uint32_t lane = 0; lane < packet->count; lane++) {
        const uint16_t bit = (uint16_t)(1u << lane);
        if (!(active & bit)) continue;
        rdp_color pixel = {
            (uint8_t)packet->color[0][lane], (uint8_t)packet->color[1][lane],
            (uint8_t)packet->color[2][lane], (uint8_t)packet->color[3][lane]
        };
        rdp_memory_pixel memory_pixel;
        const sr_result result = framebuffer_read_memory_address(memory,
            primitive->framebuffer.color_image.size, packet->color_address[lane],
            state->blend.image_read, &memory_pixel);
        if (result != SR_OK) return result;
        const uint8_t memory_coverage = memory_pixel.coverage;
        const bool overflow = ((packet->coverage[lane] + memory_coverage) & 8u) != 0u;
        const bool blend_enable = state->blend.force_blend || (state->antialias && !overflow);
        pixel = rdp_blender_evaluate(&state->blend, pixel, packet->alpha[lane],
                                     memory_pixel.color, (uint8_t)packet->shade[3][lane],
                                     blend_enable);
        uint32_t final_coverage;
        switch (state->coverage_dest & 3u) {
        case 0:
            final_coverage = blend_enable
                ? packet->coverage[lane] + memory_coverage : packet->coverage[lane] - 1u;
            if (final_coverage > 7u) final_coverage = 7u;
            break;
        case 1: final_coverage = (packet->coverage[lane] + memory_coverage) & 7u; break;
        case 2: final_coverage = 7u; break;
        default: final_coverage = memory_coverage; break;
        }
        pixel.a = (uint8_t)((final_coverage << 5) | 0x1fu);
        const uint32_t pixel_index = packet->y * primitive->framebuffer.color_image.width + packet->x[lane];
        const sr_result write_result = framebuffer_write_color_address(memory,
            primitive->framebuffer.color_image.size, packet->color_address[lane],
            pixel_index, pixel);
        if (write_result != SR_OK) return write_result;
    }
    return SR_OK;
}
