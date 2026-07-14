#include "pipeline.h"

#include <limits.h>
#include <string.h>

static int32_t interpolate_attribute(const raster_decoded_triangle *decoded,
                                     int32_t base,
                                     int32_t ddx,
                                     int32_t dde,
                                     int32_t ddy,
                                     int x,
                                     int y)
{
    const bool sign_dxhdy = decoded->position.dxhdy < 0;
    const bool do_offset = sign_dxhdy == decoded->position.flip;
    const int y_base = decoded->position.yh >> 2;
    const int dy = y - y_base;
    int64_t xh = (int64_t)decoded->position.xh + ((int64_t)dy * (int64_t)decoded->position.dxhdy);
    int32_t diff = 0;

    if (do_offset) {
        const int32_t ddeh = dde & ~0x1ff;
        const int32_t ddyh = ddy & ~0x1ff;
        xh += (int64_t)3 * (int64_t)decoded->position.dxhdy / 4;
        diff = ddeh - (ddeh >> 2) - ddyh + (ddyh >> 2);
    }

    const int base_x = (int)(xh >> 16);
    const int xfrac = (int)((xh >> 8) & 0xff);
    int64_t value = (int64_t)base + (int64_t)dde * dy;
    value = ((value & ~0x1ffll) + diff - (int64_t)xfrac * ((ddx >> 8) & ~1)) & ~0x3ffll;
    value += (int64_t)(ddx & ~0x1f) * (int64_t)(x - base_x);
    if (value < INT32_MIN) {
        return INT32_MIN;
    }
    if (value > INT32_MAX) {
        return INT32_MAX;
    }
    return (int32_t)value;
}

void pipeline_setup_triangle_span(const rdp_primitive_state *primitive,
                                  int x_begin,
                                  int x_end,
                                  int y,
                                  rdp_span_work *work)
{
    if (!primitive || !work) {
        return;
    }

    const raster_decoded_triangle *decoded = &primitive->triangle;
    memset(work, 0, sizeof(*work));
    work->x_begin = x_begin;
    work->x_end = x_end;
    work->y = y;
    work->coverage.full_x0 = x_begin;
    work->coverage.full_x1 = x_end;
    work->coverage.valid_rows = 0x0fu;

    if (decoded->has_depth) {
        work->depth_fixed = interpolate_attribute(decoded,
                                                  decoded->depth.z,
                                                  decoded->depth.dzdx,
                                                  decoded->depth.dzde,
                                                  decoded->depth.dzdy,
                                                  x_begin,
                                                  y);
    }

    if (decoded->has_texture && primitive->color.needs_texel0) {
        work->s_fixed = interpolate_attribute(decoded, decoded->texture.s,
                                              decoded->texture.dsdx,
                                              decoded->texture.dsde,
                                              decoded->texture.dsdy,
                                              x_begin, y);
        work->t_fixed = interpolate_attribute(decoded, decoded->texture.t,
                                              decoded->texture.dtdx,
                                              decoded->texture.dtde,
                                              decoded->texture.dtdy,
                                              x_begin, y);
        if (primitive->texture.perspective) {
            work->w_fixed = interpolate_attribute(decoded, decoded->texture.w,
                                                  decoded->texture.dwdx,
                                                  decoded->texture.dwde,
                                                  decoded->texture.dwdy,
                                                  x_begin, y);
        }
    }

    if (decoded->has_shade) {
        work->shade = decoded->shade;
        work->shade.r = interpolate_attribute(decoded, work->shade.r, work->shade.drdx, work->shade.drde, work->shade.drdy, x_begin, y);
        work->shade.g = interpolate_attribute(decoded, work->shade.g, work->shade.dgdx, work->shade.dgde, work->shade.dgdy, x_begin, y);
        work->shade.b = interpolate_attribute(decoded, work->shade.b, work->shade.dbdx, work->shade.dbde, work->shade.dbdy, x_begin, y);
        work->shade.a = interpolate_attribute(decoded, work->shade.a, work->shade.dadx, work->shade.dade, work->shade.dady, x_begin, y);
    }
}

void pipeline_setup_rectangle_span(int x_begin,
                                   int x_end,
                                   int y,
                                   int32_t s_fixed,
                                   int32_t t_fixed,
                                   int32_t dsdx_fixed,
                                   int32_t dtdx_fixed,
                                   rdp_span_work *work)
{
    if (!work) {
        return;
    }

    memset(work, 0, sizeof(*work));
    work->x_begin = x_begin;
    work->x_end = x_end;
    work->y = y;
    work->s_fixed = s_fixed;
    work->t_fixed = t_fixed;
    work->dsdx_fixed = dsdx_fixed;
    work->dtdx_fixed = dtdx_fixed;
}
