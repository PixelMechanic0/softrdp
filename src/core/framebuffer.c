#include "framebuffer.h"

static bool fill_rect_16(sr_memory *memory, const rdp_framebuffer_state *state, uint32_t x0, uint32_t y0, uint32_t x1, uint32_t y1)
{
    const uint32_t width = state->color_image.width;
    const uint32_t first_pixel = y0 * width + x0;
    const uint32_t last_pixel = y1 * width + x1;
    const uint32_t first_addr = state->color_image.address + first_pixel * 2u;
    const uint32_t last_addr = state->color_image.address + last_pixel * 2u + 1u;

    if (!memory->rdram || first_addr >= memory->rdram_size || last_addr >= memory->rdram_size) {
        return false;
    }

    for (uint32_t y = y0; y <= y1; y++) {
        uint32_t pixel = y * width + x0;
        uint32_t addr = state->color_image.address + pixel * 2u;
        for (uint32_t x = x0; x <= x1; x++, pixel++, addr += 2u) {
            const uint16_t value = (pixel & 1u) ? (uint16_t)state->fill_color : (uint16_t)(state->fill_color >> 16);
            memory->rdram[rdram_byte_index(memory, addr)] = (uint8_t)(value >> 8);
            memory->rdram[rdram_byte_index(memory, addr + 1u)] = (uint8_t)value;
        }
    }

    return true;
}

static bool fill_rect_32(sr_memory *memory, const rdp_framebuffer_state *state, uint32_t x0, uint32_t y0, uint32_t x1, uint32_t y1)
{
    const uint32_t width = state->color_image.width;
    const uint32_t first_pixel = y0 * width + x0;
    const uint32_t last_pixel = y1 * width + x1;
    const uint32_t first_addr = state->color_image.address + first_pixel * 4u;
    const uint32_t last_addr = state->color_image.address + last_pixel * 4u + 3u;
    uint8_t bytes[4];

    if (!memory->rdram || first_addr >= memory->rdram_size || last_addr >= memory->rdram_size) {
        return false;
    }

    if (memory->rdram_bswapped && (first_addr & 3u) != 0u) {
        for (uint32_t y = y0; y <= y1; y++) {
            uint32_t addr = state->color_image.address + (y * width + x0) * 4u;
            for (uint32_t x = x0; x <= x1; x++, addr += 4u) {
                memory->rdram[rdram_byte_index(memory, addr + 0u)] = (uint8_t)(state->fill_color >> 24);
                memory->rdram[rdram_byte_index(memory, addr + 1u)] = (uint8_t)(state->fill_color >> 16);
                memory->rdram[rdram_byte_index(memory, addr + 2u)] = (uint8_t)(state->fill_color >> 8);
                memory->rdram[rdram_byte_index(memory, addr + 3u)] = (uint8_t)state->fill_color;
            }
        }
        return true;
    }

    if (memory->rdram_bswapped) {
        bytes[0] = (uint8_t)state->fill_color;
        bytes[1] = (uint8_t)(state->fill_color >> 8);
        bytes[2] = (uint8_t)(state->fill_color >> 16);
        bytes[3] = (uint8_t)(state->fill_color >> 24);
    } else {
        bytes[0] = (uint8_t)(state->fill_color >> 24);
        bytes[1] = (uint8_t)(state->fill_color >> 16);
        bytes[2] = (uint8_t)(state->fill_color >> 8);
        bytes[3] = (uint8_t)state->fill_color;
    }

    for (uint32_t y = y0; y <= y1; y++) {
        uint32_t addr = state->color_image.address + (y * width + x0) * 4u;
        for (uint32_t x = x0; x <= x1; x++, addr += 4u) {
            memory->rdram[addr + 0u] = bytes[0];
            memory->rdram[addr + 1u] = bytes[1];
            memory->rdram[addr + 2u] = bytes[2];
            memory->rdram[addr + 3u] = bytes[3];
        }
    }

    return true;
}

sr_result framebuffer_fill_rect(sr_memory *memory, const rdp_framebuffer_state *state, uint32_t x0, uint32_t y0, uint32_t x1, uint32_t y1)
{
    if (!memory || !state || state->color_image.width == 0) return SR_ERROR_INVALID_ARGUMENT;
    if (x1 < x0 || y1 < y0 || x0 >= state->color_image.width) return SR_OK;
    if (x1 >= state->color_image.width) x1 = state->color_image.width - 1u;

    switch (state->color_image.size) {
    case RDP_SIZE_16BPP:
        return fill_rect_16(memory, state, x0, y0, x1, y1) ? SR_OK : SR_ERROR_INVALID_ARGUMENT;
    case RDP_SIZE_32BPP:
        return fill_rect_32(memory, state, x0, y0, x1, y1) ? SR_OK : SR_ERROR_INVALID_ARGUMENT;
    default:
        return SR_ERROR_UNSUPPORTED;
    }
}
