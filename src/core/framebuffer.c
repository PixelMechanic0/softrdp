#include "framebuffer.h"

static inline uint32_t byte_swap32(uint32_t value)
{
    return ((value & 0x000000ffu) << 24) |
           ((value & 0x0000ff00u) << 8) |
           ((value & 0x00ff0000u) >> 8) |
           ((value & 0xff000000u) >> 24);
}

/* The destination is 32-bit aligned before entering this loop. Keeping the
 * loop as one restrict-qualified repeated store lets the compiler vectorize
 * it without understanding RDRAM byte swapping or pixel formats. */
static void fill_repeated_words(uint8_t *rdram, uint32_t address,
                                uint32_t value, uint32_t word_count)
{
    uint32_t *restrict destination = (uint32_t *)(void *)(rdram + address);
    for (uint32_t i = 0; i < word_count; i++) destination[i] = value;
}

static bool fill_rect_8(sr_memory *memory, const rdp_framebuffer_state *state,
                        uint32_t x0, uint32_t y0, uint32_t x1, uint32_t y1)
{
    const uint32_t width = state->color_image.width;
    const uint32_t count = x1 - x0 + 1u;
    const uint32_t stored_word = memory->rdram_bswapped ? state->fill_color
                                                        : byte_swap32(state->fill_color);
    for (uint32_t y = y0; y <= y1; y++) {
        uint32_t address = state->color_image.address + y * width + x0;
        uint32_t remaining = count;
        while (remaining && (address & 3u)) {
            const uint32_t shift = (address & 3u) ^ 3u;
            if (!sr_memory_write_u8(memory, address,
                                    (uint8_t)(state->fill_color >> (shift * 8u)))) return false;
            address++;
            remaining--;
        }
        const uint32_t words = remaining >> 2;
        fill_repeated_words(memory->rdram, address, stored_word, words);
        address += words * 4u;
        remaining -= words * 4u;
        while (remaining) {
            const uint32_t shift = (address & 3u) ^ 3u;
            if (!sr_memory_write_u8(memory, address,
                                    (uint8_t)(state->fill_color >> (shift * 8u)))) return false;
            address++;
            remaining--;
        }
    }
    return true;
}

static bool fill_rect_16(sr_memory *memory, const rdp_framebuffer_state *state,
                         uint32_t x0, uint32_t y0, uint32_t x1, uint32_t y1)
{
    const uint32_t width = state->color_image.width;
    const uint32_t count = x1 - x0 + 1u;
    for (uint32_t y = y0; y <= y1; y++) {
        uint32_t pixel = y * width + x0;
        uint32_t address = state->color_image.address + pixel * 2u;
        uint32_t remaining = count;
        if (remaining && (address & 3u)) {
            const uint16_t value = (pixel & 1u) ? (uint16_t)state->fill_color
                                                : (uint16_t)(state->fill_color >> 16);
            if (!sr_memory_write_be16(memory, address, value)) return false;
            pixel++;
            address += 2u;
            remaining--;
        }
        uint32_t pair = (pixel & 1u) ?
                        (state->fill_color << 16) | (state->fill_color >> 16) :
                        state->fill_color;
        if (!memory->rdram_bswapped) pair = byte_swap32(pair);
        const uint32_t words = remaining >> 1;
        fill_repeated_words(memory->rdram, address, pair, words);
        pixel += words * 2u;
        address += words * 4u;
        remaining -= words * 2u;
        if (remaining) {
            const uint16_t value = (pixel & 1u) ? (uint16_t)state->fill_color
                                                : (uint16_t)(state->fill_color >> 16);
            if (!sr_memory_write_be16(memory, address, value)) return false;
        }
    }
    return true;
}

static bool fill_rect_32(sr_memory *memory, const rdp_framebuffer_state *state,
                         uint32_t x0, uint32_t y0, uint32_t x1, uint32_t y1)
{
    const uint32_t width = state->color_image.width;
    const uint32_t count = x1 - x0 + 1u;
    const uint32_t stored_word = memory->rdram_bswapped ? state->fill_color
                                                        : byte_swap32(state->fill_color);
    for (uint32_t y = y0; y <= y1; y++) {
        const uint32_t address = state->color_image.address +
                                 (y * width + x0) * 4u;
        if (address & 3u) {
            for (uint32_t x = 0; x < count; x++)
                if (!sr_memory_write_be32(memory, address + x * 4u,
                                          state->fill_color)) return false;
        } else {
            fill_repeated_words(memory->rdram, address, stored_word, count);
        }
    }
    return true;
}

sr_result framebuffer_fill_rect(sr_memory *memory, const rdp_framebuffer_state *state,
                                uint32_t x0, uint32_t y0, uint32_t x1, uint32_t y1)
{
    if (!memory || !state || !memory->rdram || state->color_image.width == 0)
        return SR_ERROR_INVALID_ARGUMENT;
    if (x1 < x0 || y1 < y0 || x0 >= state->color_image.width) return SR_OK;
    if (x1 >= state->color_image.width) x1 = state->color_image.width - 1u;

    if (state->color_image.size < RDP_SIZE_8BPP ||
        state->color_image.size > RDP_SIZE_32BPP) return SR_ERROR_UNSUPPORTED;
    const uint32_t bytes_per_pixel = 1u << (state->color_image.size - 1u);
    const uint64_t first_pixel = (uint64_t)y0 * state->color_image.width + x0;
    const uint64_t last_pixel = (uint64_t)y1 * state->color_image.width + x1;
    const uint64_t first_address = state->color_image.address + first_pixel * bytes_per_pixel;
    const uint64_t last_address = state->color_image.address +
                                  last_pixel * bytes_per_pixel + bytes_per_pixel - 1u;
    if (first_address >= memory->rdram_size || last_address >= memory->rdram_size)
        return SR_ERROR_INVALID_ARGUMENT;

    bool success;
    switch (state->color_image.size) {
    case RDP_SIZE_8BPP: success = fill_rect_8(memory, state, x0, y0, x1, y1); break;
    case RDP_SIZE_16BPP: success = fill_rect_16(memory, state, x0, y0, x1, y1); break;
    case RDP_SIZE_32BPP: success = fill_rect_32(memory, state, x0, y0, x1, y1); break;
    default: return SR_ERROR_UNSUPPORTED;
    }
    return success ? SR_OK : SR_ERROR_INVALID_ARGUMENT;
}
