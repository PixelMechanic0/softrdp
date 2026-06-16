#include "framebuffer.h"

sr_result framebuffer_fill_rect(sr_memory *memory, const rdp_state *state, uint32_t x0, uint32_t y0, uint32_t x1, uint32_t y1)
{
    if (!memory || !state || state->color_image.width == 0) return SR_ERROR_INVALID_ARGUMENT;
    if (x1 < x0 || y1 < y0 || x0 >= state->color_image.width) return SR_OK;
    if (x1 >= state->color_image.width) x1 = state->color_image.width - 1u;

    switch (state->color_image.size) {
    case RDP_SIZE_16BPP:
        for (uint32_t y = y0; y <= y1; y++) {
            for (uint32_t x = x0; x <= x1; x++) {
                if (!write_fill_pixel_16(memory, state, x, y)) return SR_ERROR_INVALID_ARGUMENT;
            }
        }
        break;
    case RDP_SIZE_32BPP:
        for (uint32_t y = y0; y <= y1; y++) {
            for (uint32_t x = x0; x <= x1; x++) {
                if (!write_fill_pixel_32(memory, state, x, y)) return SR_ERROR_INVALID_ARGUMENT;
            }
        }
        break;
    default:
        return SR_ERROR_UNSUPPORTED;
    }
    return SR_OK;
}
