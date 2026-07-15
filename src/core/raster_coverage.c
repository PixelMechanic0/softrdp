#include "raster_coverage.h"

static inline uint8_t coverage_popcount(uint8_t mask)
{
#if defined(__GNUC__) || defined(__clang__)
    return (uint8_t)__builtin_popcount((unsigned)mask);
#else
    mask = (uint8_t)(mask - ((mask >> 1) & 0x55u));
    mask = (uint8_t)((mask & 0x33u) + ((mask >> 2) & 0x33u));
    return (uint8_t)((mask + (mask >> 4)) & 0x0fu);
#endif
}

raster_coverage raster_coverage_evaluate(const raster_coverage_span *span, int x)
{
    raster_coverage result = {0};
    if (x >= span->full_x0 && x <= span->full_x1) {
        result.mask = 0xffu;
        result.count = 8u;
        result.first_x_eighth = -4;
        result.first_y_eighth = -4;
        return result;
    }

    /* A staggered 8-sample grid. Coordinates are eighth-pixels;
     * only boundary pixels execute this loop. */
    static const int8_t sample_x[4][2] = {
        { 0, 4 }, { 2, 6 }, { 0, 4 }, { 2, 6 }
    };
    const int32_t pixel_base = x << 16;
    uint8_t mask = 0u;
    for (uint32_t row = 0; row < 4u; row++) {
        if (!(span->valid_rows & (1u << row))) continue;
        for (uint32_t sample = 0; sample < 2u; sample++) {
            const int32_t position = pixel_base + (int32_t)sample_x[row][sample] * 8192;
            if (position >= span->left[row] && position < span->right[row])
                mask |= (uint8_t)(1u << (row * 2u + sample));
        }
    }
    result.mask = mask;
    result.count = coverage_popcount(mask);
    if (mask) {
#if defined(__GNUC__) || defined(__clang__)
        const uint32_t first = (uint32_t)__builtin_ctz((unsigned)mask);
#else
        uint32_t first = 0u;
        while (!(mask & (uint8_t)(1u << first))) first++;
#endif
        const uint32_t row = first >> 1;
        const uint32_t sample = first & 1u;
        result.first_x_eighth = (int8_t)(sample_x[row][sample] - 4);
        result.first_y_eighth = (int8_t)((int32_t)row * 2 - 4);
    }
    return result;
}
