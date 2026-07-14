#include "fragment.h"

#include "blender.h"
#include "framebuffer.h"

static uint8_t dither_component(uint8_t value, uint8_t threshold)
{
    if ((value & 7u) <= threshold) return value;
    return value > 247u ? 255u : (uint8_t)((value & 0xf8u) + 8u);
}

static rdp_color dither_rgb(rdp_color color, uint8_t mode, uint32_t x, uint32_t y)
{
    static const uint8_t magic[16] = {
        0, 6, 1, 7, 4, 2, 5, 3, 3, 5, 2, 4, 7, 1, 6, 0
    };
    static const uint8_t bayer[16] = {
        0, 4, 1, 5, 4, 0, 5, 1, 3, 7, 2, 6, 7, 3, 6, 2
    };
    if (mode == 3u) return color;
    const uint32_t index = ((y & 3u) << 2) | (x & 3u);
    if (mode == 2u) {
        const uint32_t noise = (x * 0x1f123bb5u + y * 0x159a55e5u + 0x9e3779b9u);
        color.r = dither_component(color.r, (uint8_t)(noise & 7u));
        color.g = dither_component(color.g, (uint8_t)((noise >> 3) & 7u));
        color.b = dither_component(color.b, (uint8_t)((noise >> 6) & 7u));
    } else {
        const uint8_t threshold = mode == 0u ? magic[index] : bayer[index];
        color.r = dither_component(color.r, threshold);
        color.g = dither_component(color.g, threshold);
        color.b = dither_component(color.b, threshold);
    }
    return color;
}

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
        packet->alpha[lane] = packet->color[3][lane];
        if (!(active & bit)) continue;
        if (state->cvg_times_alpha) {
            packet->alpha[lane] = (uint16_t)(((uint32_t)packet->alpha[lane] * packet->coverage[lane] + 4u) >> 3);
            packet->coverage[lane] = (uint8_t)((packet->alpha[lane] >> 5) & 0xfu);
            if (packet->coverage[lane] == 0u) {
                active &= (uint16_t)~bit;
                continue;
            }
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
        if (primitive->framebuffer.color_image.size == RDP_SIZE_16BPP)
            pixel = dither_rgb(pixel, state->rgb_dither, packet->x[lane], packet->y);
        const sr_result write_result = framebuffer_write_color_address(memory,
            primitive->framebuffer.color_image.size, packet->color_address[lane],
            pixel_index, pixel);
        if (write_result != SR_OK) return write_result;
    }
    return SR_OK;
}
