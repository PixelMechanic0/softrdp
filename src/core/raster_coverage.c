#include "raster_coverage.h"

raster_coverage raster_coverage_evaluate(const raster_coverage_span *span, int x)
{
    raster_coverage result = {0};
    if (x >= span->full_x0 && x <= span->full_x1) {
        result.count = 8u;
        result.center_covered = true;
        return result;
    }

    /* A staggered 8-sample grid. Coordinates are eighth-pixels;
     * only boundary pixels execute this loop. */
    static const int8_t sample_x[4][2] = {
        { 0, 4 }, { 2, 6 }, { 0, 4 }, { 2, 6 }
    };
    static const int8_t sample_y[4] = { 0, 2, 4, 6 };
    const int32_t pixel_base = x << 16;
    for (uint32_t row = 0; row < 4u; row++) {
        if (!(span->valid_rows & (1u << row))) continue;
        for (uint32_t sample = 0; sample < 2u; sample++) {
            const int32_t position = pixel_base + (int32_t)sample_x[row][sample] * 8192;
            if (position >= span->left[row] && position < span->right[row]) {
                result.count++;
                result.centroid_x_sum += (int8_t)(sample_x[row][sample] - 4);
                result.centroid_y_sum += (int8_t)(sample_y[row] - 4);
            }
        }
    }

    /* Non-antialiased rendering uses the geometric pixel center, independent
     * of which multisample bit happened to be assigned first. */
    const int32_t center = pixel_base + 0x8000;
    result.center_covered = (span->valid_rows & (1u << 2)) != 0u &&
        center >= span->left[2] && center < span->right[2];
    return result;
}
