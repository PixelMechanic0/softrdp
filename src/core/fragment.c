#include "fragment.h"

#include "blender.h"
#include "framebuffer.h"

sr_result fragment_finish_packet(sr_memory *memory,
                                 const rdp_primitive_state *primitive,
                                 rdp_fragment_packet *packet)
{
    if (!memory || !primitive || !packet || packet->count > RDP_PACKET_LANES)
        return SR_ERROR_INVALID_ARGUMENT;

    const rdp_fragment_state *state = &primitive->fragment;
    uint16_t active = packet->active_mask;
    uint16_t alpha[RDP_PACKET_LANES];
    uint8_t coverage[RDP_PACKET_LANES];
    uint8_t memory_coverage[RDP_PACKET_LANES];
    uint8_t blend_enable[RDP_PACKET_LANES];
    rdp_color pixel[RDP_PACKET_LANES];
    rdp_color memory_color[RDP_PACKET_LANES];

    /* Alpha and coverage are pure packet arithmetic. Rejected lanes remain in
     * the packet and are removed from the shared active mask. */
    for (uint32_t lane = 0; lane < packet->count; lane++) {
        const uint16_t bit = (uint16_t)(1u << lane);
        coverage[lane] = 8u;
        alpha[lane] = packet->color[3][lane];
        if (!(active & bit)) continue;
        if (state->cvg_times_alpha) {
            alpha[lane] = (uint16_t)(((uint32_t)alpha[lane] * coverage[lane] + 4u) >> 3);
            coverage[lane] = (uint8_t)((alpha[lane] >> 5) & 0xfu);
        }
        if (state->alpha_cvg_select && !state->cvg_times_alpha)
            alpha[lane] = (uint16_t)coverage[lane] << 5;
        if (state->blend.alpha_compare && alpha[lane] < state->blend.blend_color.a) {
            active &= (uint16_t)~bit;
        }
    }

    /* The framebuffer codec owns format conversion. The fragment pipeline
     * consumes one uniform memory-color representation for every format. */
    for (uint32_t lane = 0; lane < packet->count; lane++) {
        const uint16_t bit = (uint16_t)(1u << lane);
        if (!(active & bit)) continue;
        pixel[lane] = (rdp_color){
            (uint8_t)packet->color[0][lane], (uint8_t)packet->color[1][lane],
            (uint8_t)packet->color[2][lane], (uint8_t)packet->color[3][lane]
        };
        rdp_memory_pixel memory_pixel;
        const sr_result result = framebuffer_read_memory_pixel(memory,
            &primitive->framebuffer, packet->x[lane], packet->y[lane],
            state->blend.image_read, &memory_pixel);
        if (result != SR_OK) return result;
        memory_color[lane] = memory_pixel.color;
        memory_coverage[lane] = memory_pixel.coverage;
        const bool overflow = ((coverage[lane] + memory_coverage[lane]) & 8u) != 0u;
        blend_enable[lane] = state->blend.force_blend || (state->antialias && !overflow);
    }

    /* Both blender cycles and all source selections are handled by the common
     * decoded blender. No framebuffer/cycle/mode combination creates a new
     * rendering path here. */
    for (uint32_t lane = 0; lane < packet->count; lane++) {
        if (!(active & (uint16_t)(1u << lane))) continue;
        pixel[lane] = rdp_blender_evaluate(&state->blend, pixel[lane], alpha[lane],
                                           memory_color[lane],
                                           (uint8_t)packet->shade_alpha[lane],
                                           blend_enable[lane] != 0u);
    }

    packet->accepted_mask = active;
    for (uint32_t lane = 0; lane < packet->count; lane++) {
        if (!(active & (uint16_t)(1u << lane))) continue;
        uint32_t final_coverage;
        switch (state->coverage_dest & 3u) {
        case 0:
            final_coverage = blend_enable[lane]
                ? coverage[lane] + memory_coverage[lane] : coverage[lane] - 1u;
            if (final_coverage > 7u) final_coverage = 7u;
            break;
        case 1: final_coverage = (coverage[lane] + memory_coverage[lane]) & 7u; break;
        case 2: final_coverage = 7u; break;
        default: final_coverage = memory_coverage[lane]; break;
        }
        pixel[lane].a = (uint8_t)((final_coverage << 5) | 0x1fu);
        const sr_result result = framebuffer_write_color(memory, &primitive->framebuffer,
            packet->x[lane], packet->y[lane], pixel[lane]);
        if (result != SR_OK) return result;
    }
    return SR_OK;
}
