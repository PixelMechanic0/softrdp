extern "C" {
#include "fragment.h"
#include "blender.h"
#include "framebuffer.h"
}

/* Shared by RGB and alpha dither. Index 0 is the "magic square" pattern used by
 * even dither modes, index 1 the Bayer pattern used by odd modes. */
static const uint8_t dither_matrix_magic[16] = {
    0, 6, 1, 7, 4, 2, 5, 3, 3, 5, 2, 4, 7, 1, 6, 0
};
static const uint8_t dither_matrix_bayer[16] = {
    0, 4, 1, 5, 4, 0, 5, 1, 3, 7, 2, 6, 7, 3, 6, 2
};

static uint8_t dither_component(uint8_t value, uint8_t threshold)
{
    if ((value & 7u) <= threshold) return value;
    return value > 247u ? 255u : (uint8_t)((value & 0xf8u) + 8u);
}

static rdp_color dither_rgb(rdp_color color, uint8_t mode, uint32_t x, uint32_t y,
                            uint32_t primitive)
{
    if (mode == 3u) return color;
    const uint32_t index = ((y & 3u) << 2) | (x & 3u);
    if (mode == 2u) {
        const uint32_t noise = rdp_pixel_noise(x, y, primitive);
        color.r = dither_component(color.r, (uint8_t)(noise & 7u));
        color.g = dither_component(color.g, (uint8_t)((noise >> 3) & 7u));
        color.b = dither_component(color.b, (uint8_t)((noise >> 6) & 7u));
    } else {
        const uint8_t threshold = mode == 0u ? dither_matrix_magic[index]
                                             : dither_matrix_bayer[index];
        color.r = dither_component(color.r, threshold);
        color.g = dither_component(color.g, threshold);
        color.b = dither_component(color.b, threshold);
    }
    return color;
}

template <rdp_texture_size Size>
static sr_result finish_color_lanes(sr_memory *memory,
                                    const rdp_primitive_state *primitive,
                                    rdp_fragment_block *packet,
                                    uint16_t active)
{
    const rdp_fragment_state *state = &primitive->fragment;
    for (uint32_t lane = 0; lane < packet->count; lane++) {
        const uint16_t bit = (uint16_t)(1u << lane);
        if (!(active & bit)) continue;
        rdp_color pixel = {
            (uint8_t)packet->color[0][lane], (uint8_t)packet->color[1][lane],
            (uint8_t)packet->color[2][lane], (uint8_t)packet->color[3][lane]
        };
        rdp_memory_pixel memory_pixel;
        const sr_result result = framebuffer_read_memory_address(
            memory, Size, packet->color_address[lane],
            state->blend.image_read, &memory_pixel);
        if (result != SR_OK) return result;
        const uint8_t memory_coverage = memory_pixel.coverage;
        const bool overflow = ((packet->coverage[lane] + memory_coverage) & 8u) != 0u;
        const bool blend_enable = state->blend.force_blend ||
                                  (state->antialias && !overflow);
        /* COLOR_ON_CVG affects only the final blender cycle. In two-cycle
         * mode cycle 0 must still produce the final cycle's pixel input. */
        pixel = rdp_blender_evaluate_coverage(
            &state->blend, pixel, packet->alpha[lane], memory_pixel.color,
            (uint8_t)packet->shade[3][lane], blend_enable,
            state->color_on_cvg, overflow);
        uint32_t final_coverage;
        switch (state->coverage_dest & 3u) {
        case 0:
            final_coverage = blend_enable
                ? packet->coverage[lane] + memory_coverage
                : packet->coverage[lane] - 1u;
            if (final_coverage > 7u) final_coverage = 7u;
            break;
        case 1: final_coverage = (packet->coverage[lane] + memory_coverage) & 7u; break;
        case 2: final_coverage = 7u; break;
        default: final_coverage = memory_coverage; break;
        }
        pixel.a = (uint8_t)((final_coverage << 5) | 0x1fu);
        const uint32_t pixel_index = packet->y *
            primitive->framebuffer.color_image.width + packet->x[lane];
        if constexpr (Size == RDP_SIZE_16BPP)
            pixel = dither_rgb(pixel, state->rgb_dither, packet->x[lane], packet->y,
                               primitive->color.primitive_counter);
        const sr_result write_result = framebuffer_write_color_address(
            memory, Size, packet->color_address[lane], pixel_index, pixel);
        if (write_result != SR_OK) return write_result;
    }
    return SR_OK;
}

extern "C"
sr_result fragment_finish_packet(sr_memory *memory,
                                 const rdp_primitive_state *primitive,
                                 rdp_fragment_block *packet)
{
    if (!memory || !primitive || !packet || packet->count > RDP_PACKET_LANES)
        return SR_ERROR_INVALID_ARGUMENT;

    const rdp_fragment_state *state = &primitive->fragment;
    uint16_t active = packet->active_mask;

    /* Alpha dither biases combiner alpha before the blender and alpha compare.
     * Modes 0/1 use a matrix chosen by the low bit of the RGB dither mode: magic
     * for even modes, Bayer for odd, with mode 1 inverting the value. Mode 2 uses
     * per-pixel noise, mode 3 disables it. It applies only when alpha is not
     * selected from coverage (alpha_cvg_select). */
    const uint8_t *alpha_dither_matrix = NULL;
    bool alpha_dither_invert = false;
    const bool alpha_dither_noise = state->alpha_dither == 2u;
    if (state->alpha_dither < 2u) {
        alpha_dither_matrix = (state->rgb_dither & 1u) ? dither_matrix_bayer
                                                       : dither_matrix_magic;
        alpha_dither_invert = state->alpha_dither == 1u;
    }
    const uint32_t primitive_counter = primitive->color.primitive_counter;

    /* Alpha and coverage are pure packet arithmetic. Rejected lanes remain in
     * the packet and are removed from the shared active mask. */
    for (uint32_t lane = 0; lane < packet->count; lane++) {
        const uint16_t bit = (uint16_t)(1u << lane);
        packet->alpha[lane] = packet->color[3][lane];
        if (!(active & bit)) continue;
        if (state->cvg_times_alpha) {
            packet->alpha[lane] = (uint16_t)(((uint32_t)packet->alpha[lane] * packet->coverage[lane] + 4u) >> 3);
            packet->coverage[lane] = (uint8_t)((packet->alpha[lane] >> 5) & 0xfu);
            /* With antialiasing disabled the blender gates writes with the
             * original coverage bit, not the alpha-scaled coverage count. */
            if (state->antialias && packet->coverage[lane] == 0u) {
                active &= (uint16_t)~bit;
                continue;
            }
        }
        if (state->alpha_cvg_select) {
            if (!state->cvg_times_alpha)
                packet->alpha[lane] = (uint16_t)packet->coverage[lane] << 5;
            if (packet->alpha[lane] > 0xffu)
                packet->alpha[lane] = 0xffu;
        } else if (alpha_dither_matrix || alpha_dither_noise) {
            uint8_t adith;
            if (alpha_dither_noise) {
                adith = (uint8_t)(rdp_pixel_noise(packet->x[lane], packet->y,
                                                  primitive_counter) & 7u);
            } else {
                const uint32_t index = ((packet->y & 3u) << 2) | (packet->x[lane] & 3u);
                adith = alpha_dither_matrix[index];
                if (alpha_dither_invert) adith = (uint8_t)(~adith & 7u);
            }
            const uint32_t dithered = (uint32_t)packet->alpha[lane] + adith;
            packet->alpha[lane] = dithered > 0xffu ? 0xffu : (uint16_t)dithered;
        }
        if (state->blend.alpha_compare) {
            /* The alpha-compare threshold is normally the blend colour alpha, but
             * dithered alpha compare replaces it with a per-pixel noise value. */
            const uint8_t threshold = state->blend.alpha_compare_dither
                ? (uint8_t)(rdp_pixel_noise(packet->x[lane], packet->y,
                                            primitive_counter) & 0xffu)
                : state->blend.blend_color.a;
            if (packet->alpha[lane] < threshold)
                active &= (uint16_t)~bit;
        }
    }

    packet->accepted_mask = active;
    /* Framebuffer format is constant for the primitive. Dispatch once per
     * packet so reads, dithering, packing, and writes specialize together. */
    switch (primitive->framebuffer.color_image.size) {
    case RDP_SIZE_8BPP:
        return finish_color_lanes<RDP_SIZE_8BPP>(memory, primitive, packet, active);
    case RDP_SIZE_16BPP:
        return finish_color_lanes<RDP_SIZE_16BPP>(memory, primitive, packet, active);
    case RDP_SIZE_32BPP:
        return finish_color_lanes<RDP_SIZE_32BPP>(memory, primitive, packet, active);
    default:
        return SR_ERROR_UNSUPPORTED;
    }
}
