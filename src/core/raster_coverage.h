#ifndef RASTER_COVERAGE_H
#define RASTER_COVERAGE_H

#include <stdbool.h>
#include <stdint.h>

typedef struct raster_coverage_span {
    int32_t left[4];
    int32_t right[4];
    int full_x0;
    int full_x1;
    uint8_t valid_rows;
} raster_coverage_span;

typedef struct raster_coverage {
    uint8_t count;
    bool center_covered;
    int8_t centroid_x_sum;
    int8_t centroid_y_sum;
} raster_coverage;

raster_coverage raster_coverage_evaluate(const raster_coverage_span *span, int x);

#endif
