#ifndef FRAMEBUFFER_H
#define FRAMEBUFFER_H

#include "rdp_memory.h"
#include "rdp_state.h"
#include <stdbool.h>
#include <stdint.h>

sr_result framebuffer_fill_rect(sr_memory *memory,
                                const rdp_framebuffer_state *state,
                                uint32_t x0,
                                uint32_t y0,
                                uint32_t x1,
                                uint32_t y1);

static inline bool write_fill_pixel_16(sr_memory *memory, const rdp_framebuffer_state *state, uint32_t x, uint32_t y)
{
    const uint32_t pixel = y * state->color_image.width + x;
    const uint16_t value = (pixel & 1u) ? (uint16_t)state->fill_color : (uint16_t)(state->fill_color >> 16);
    return sr_memory_write_be16(memory, state->color_image.address + pixel * 2u, value);
}

static inline bool write_fill_pixel_32(sr_memory *memory, const rdp_framebuffer_state *state, uint32_t x, uint32_t y)
{
    const uint32_t pixel = y * state->color_image.width + x;
    return sr_memory_write_be32(memory, state->color_image.address + pixel * 4u, state->fill_color);
}

static inline sr_result framebuffer_write_rgba5551(sr_memory *memory, const rdp_framebuffer_state *state, uint32_t x, uint32_t y, uint16_t texel)
{
    if (!memory || !state || x >= state->color_image.width) return SR_ERROR_INVALID_ARGUMENT;
    const uint32_t pixel = y * state->color_image.width + x;
    switch (state->color_image.size) {
    case RDP_SIZE_16BPP: return sr_memory_write_be16(memory, state->color_image.address + pixel * 2u, texel) ? SR_OK : SR_ERROR_INVALID_ARGUMENT;
    case RDP_SIZE_32BPP: return sr_memory_write_be32(memory, state->color_image.address + pixel * 4u, pipeline_color_to_rgba8888(pipeline_rgba5551_to_color(texel))) ? SR_OK : SR_ERROR_INVALID_ARGUMENT;
    default:             return SR_ERROR_UNSUPPORTED;
    }
}

static inline sr_result framebuffer_write_color(sr_memory *memory, const rdp_framebuffer_state *state, uint32_t x, uint32_t y, rdp_color color)
{
    if (!memory || !state || x >= state->color_image.width) return SR_ERROR_INVALID_ARGUMENT;
    const uint32_t pixel = y * state->color_image.width + x;
    switch (state->color_image.size) {
    case RDP_SIZE_16BPP: return sr_memory_write_be16(memory, state->color_image.address + pixel * 2u, pipeline_color_to_rgba5551(color)) ? SR_OK : SR_ERROR_INVALID_ARGUMENT;
    case RDP_SIZE_32BPP: return sr_memory_write_be32(memory, state->color_image.address + pixel * 4u, pipeline_color_to_rgba8888(color)) ? SR_OK : SR_ERROR_INVALID_ARGUMENT;
    default:             return SR_ERROR_UNSUPPORTED;
    }
}

static inline sr_result framebuffer_write_fill_pixel(sr_memory *memory, const rdp_framebuffer_state *state, uint32_t x, uint32_t y)
{
    if (!memory || !state || x >= state->color_image.width) return SR_ERROR_INVALID_ARGUMENT;
    switch (state->color_image.size) {
    case RDP_SIZE_16BPP: return write_fill_pixel_16(memory, state, x, y) ? SR_OK : SR_ERROR_INVALID_ARGUMENT;
    case RDP_SIZE_32BPP: return write_fill_pixel_32(memory, state, x, y) ? SR_OK : SR_ERROR_INVALID_ARGUMENT;
    default:             return SR_ERROR_UNSUPPORTED;
    }
}

#endif
