#ifndef FRAMEBUFFER_H
#define FRAMEBUFFER_H

#include "rdp_memory.h"
#include "rdp_state.h"

typedef struct rdp_memory_pixel {
    rdp_color color;
    uint8_t coverage;
} rdp_memory_pixel;
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
    case RDP_SIZE_8BPP: {
        const rdp_color color = pipeline_rgba5551_to_color(texel);
        const uint8_t value = (pixel & 1u) ? color.g : color.r;
        return sr_memory_write_u8(memory, state->color_image.address + pixel, value) ? SR_OK : SR_ERROR_INVALID_ARGUMENT;
    }
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
    /* The 8-bit framebuffer packs the red and green blender outputs on the
     * two pixel lanes.  This is observable for scratch images. */
    case RDP_SIZE_8BPP: {
        const uint8_t value = (pixel & 1u) ? color.g : color.r;
        return sr_memory_write_u8(memory, state->color_image.address + pixel, value) ? SR_OK : SR_ERROR_INVALID_ARGUMENT;
    }
    case RDP_SIZE_16BPP: return sr_memory_write_be16(memory, state->color_image.address + pixel * 2u, pipeline_color_to_rgba5551(color)) ? SR_OK : SR_ERROR_INVALID_ARGUMENT;
    case RDP_SIZE_32BPP: return sr_memory_write_be32(memory, state->color_image.address + pixel * 4u, pipeline_color_to_rgba8888(color)) ? SR_OK : SR_ERROR_INVALID_ARGUMENT;
    default:             return SR_ERROR_UNSUPPORTED;
    }
}

static inline sr_result framebuffer_read_color(sr_memory *memory, const rdp_framebuffer_state *state,
                                                uint32_t x, uint32_t y, rdp_color *color)
{
    if (!memory || !state || !color || x >= state->color_image.width) return SR_ERROR_INVALID_ARGUMENT;
    const uint32_t pixel = y * state->color_image.width + x;
    if (state->color_image.size == RDP_SIZE_8BPP) {
        uint8_t value;
        if (!sr_memory_read_u8(memory, state->color_image.address + pixel, &value)) return SR_ERROR_INVALID_ARGUMENT;
        *color = (rdp_color){ value, value, value, 0xe0u };
        return SR_OK;
    }
    if (state->color_image.size == RDP_SIZE_16BPP) {
        uint16_t value;
        if (!sr_memory_read_be16(memory, state->color_image.address + pixel * 2u, &value)) return SR_ERROR_INVALID_ARGUMENT;
        *color = pipeline_rgba5551_to_color(value);
        color->a &= 0xe0u;
        return SR_OK;
    }
    if (state->color_image.size == RDP_SIZE_32BPP) {
        uint32_t value;
        if (!sr_memory_read_be32(memory, state->color_image.address + pixel * 4u, &value)) return SR_ERROR_INVALID_ARGUMENT;
        *color = (rdp_color){ (uint8_t)(value >> 24), (uint8_t)(value >> 16),
                              (uint8_t)(value >> 8), (uint8_t)value & 0xe0u };
        return SR_OK;
    }
    return SR_ERROR_UNSUPPORTED;
}

static inline sr_result framebuffer_read_memory_pixel(sr_memory *memory,
                                                       const rdp_framebuffer_state *state,
                                                       uint32_t x, uint32_t y,
                                                       bool image_read,
                                                       rdp_memory_pixel *pixel)
{
    if (!pixel) return SR_ERROR_INVALID_ARGUMENT;
    pixel->color = (rdp_color){0, 0, 0, 0xe0u};
    pixel->coverage = 7u;
    if (!image_read) return SR_OK;
    const sr_result result = framebuffer_read_color(memory, state, x, y, &pixel->color);
    if (result != SR_OK) return result;
    pixel->coverage = (pixel->color.a >> 5) & 7u;
    return SR_OK;
}

static inline sr_result framebuffer_write_fill_pixel(sr_memory *memory, const rdp_framebuffer_state *state, uint32_t x, uint32_t y)
{
    if (!memory || !state || x >= state->color_image.width) return SR_ERROR_INVALID_ARGUMENT;
    switch (state->color_image.size) {
    case RDP_SIZE_8BPP: {
        const uint32_t pixel = y * state->color_image.width + x;
        const uint32_t shift = ((state->color_image.address + pixel) & 3u) ^ 3u;
        const uint8_t value = (uint8_t)(state->fill_color >> (shift * 8u));
        return sr_memory_write_u8(memory, state->color_image.address + pixel, value) ? SR_OK : SR_ERROR_INVALID_ARGUMENT;
    }
    case RDP_SIZE_16BPP: return write_fill_pixel_16(memory, state, x, y) ? SR_OK : SR_ERROR_INVALID_ARGUMENT;
    case RDP_SIZE_32BPP: return write_fill_pixel_32(memory, state, x, y) ? SR_OK : SR_ERROR_INVALID_ARGUMENT;
    default:             return SR_ERROR_UNSUPPORTED;
    }
}

#endif
