#include "framebuffer.h"

#include "pipeline.h"

static bool write_fill_pixel_16(sr_memory *memory, const rdp_state *state, uint32_t x, uint32_t y)
{
    const uint32_t pixel = y * state->color_image.width + x;
    const uint32_t addr = state->color_image.address + pixel * 2u;
    uint16_t value;

    if (pixel & 1u) {
        value = (uint16_t)state->fill_color;
    } else {
        value = (uint16_t)(state->fill_color >> 16);
    }

    return sr_memory_write_be16(memory, addr, value);
}

static bool write_fill_pixel_32(sr_memory *memory, const rdp_state *state, uint32_t x, uint32_t y)
{
    const uint32_t pixel = y * state->color_image.width + x;
    const uint32_t addr = state->color_image.address + pixel * 4u;
    return sr_memory_write_be32(memory, addr, state->fill_color);
}

sr_result framebuffer_fill_rect(sr_memory *memory,
                                const rdp_state *state,
                                uint32_t x0,
                                uint32_t y0,
                                uint32_t x1,
                                uint32_t y1)
{
    if (!memory || !state || state->color_image.width == 0) {
        return SR_ERROR_INVALID_ARGUMENT;
    }

    if (x1 < x0 || y1 < y0) {
        return SR_OK;
    }

    if (x0 >= state->color_image.width) {
        return SR_OK;
    }

    if (x1 >= state->color_image.width) {
        x1 = state->color_image.width - 1u;
    }

    for (uint32_t y = y0; y <= y1; y++) {
        for (uint32_t x = x0; x <= x1; x++) {
            bool ok;

            switch (state->color_image.size) {
            case RDP_SIZE_16BPP:
                ok = write_fill_pixel_16(memory, state, x, y);
                break;
            case RDP_SIZE_32BPP:
                ok = write_fill_pixel_32(memory, state, x, y);
                break;
            default:
                return SR_ERROR_UNSUPPORTED;
            }

            if (!ok) {
                return SR_ERROR_INVALID_ARGUMENT;
            }
        }
    }

    return SR_OK;
}

sr_result framebuffer_write_rgba5551(sr_memory *memory,
                                     const rdp_state *state,
                                     uint32_t x,
                                     uint32_t y,
                                     uint16_t texel)
{
    if (!memory || !state || x >= state->color_image.width) {
        return SR_ERROR_INVALID_ARGUMENT;
    }

    const uint32_t pixel = y * state->color_image.width + x;
    switch (state->color_image.size) {
    case RDP_SIZE_16BPP:
        return sr_memory_write_be16(memory, state->color_image.address + pixel * 2u, texel) ?
               SR_OK : SR_ERROR_INVALID_ARGUMENT;

    case RDP_SIZE_32BPP:
        return sr_memory_write_be32(memory, state->color_image.address + pixel * 4u,
                                    pipeline_color_to_rgba8888(pipeline_rgba5551_to_color(texel))) ?
               SR_OK : SR_ERROR_INVALID_ARGUMENT;

    default:
        return SR_ERROR_UNSUPPORTED;
    }
}

sr_result framebuffer_write_color(sr_memory *memory,
                                  const rdp_state *state,
                                  uint32_t x,
                                  uint32_t y,
                                  rdp_color color)
{
    if (!memory || !state || x >= state->color_image.width) {
        return SR_ERROR_INVALID_ARGUMENT;
    }

    const uint32_t pixel = y * state->color_image.width + x;
    switch (state->color_image.size) {
    case RDP_SIZE_16BPP:
        return sr_memory_write_be16(memory, state->color_image.address + pixel * 2u,
                                    pipeline_color_to_rgba5551(color)) ?
               SR_OK : SR_ERROR_INVALID_ARGUMENT;

    case RDP_SIZE_32BPP:
        return sr_memory_write_be32(memory, state->color_image.address + pixel * 4u,
                                    pipeline_color_to_rgba8888(color)) ?
               SR_OK : SR_ERROR_INVALID_ARGUMENT;

    default:
        return SR_ERROR_UNSUPPORTED;
    }
}

sr_result framebuffer_write_fill_pixel(sr_memory *memory,
                                       const rdp_state *state,
                                       uint32_t x,
                                       uint32_t y)
{
    if (!memory || !state || x >= state->color_image.width) {
        return SR_ERROR_INVALID_ARGUMENT;
    }

    switch (state->color_image.size) {
    case RDP_SIZE_16BPP:
        return write_fill_pixel_16(memory, state, x, y) ? SR_OK : SR_ERROR_INVALID_ARGUMENT;
    case RDP_SIZE_32BPP:
        return write_fill_pixel_32(memory, state, x, y) ? SR_OK : SR_ERROR_INVALID_ARGUMENT;
    default:
        return SR_ERROR_UNSUPPORTED;
    }
}
