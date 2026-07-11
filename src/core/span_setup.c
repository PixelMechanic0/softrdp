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

static int32_t texture_interpolated_value(int32_t base,
                                          int32_t ddx,
                                          int32_t ddy,
                                          int64_t dx_fixed,
                                          int64_t dy_fixed)
{
    return (int32_t)((int64_t)base +
                     ((int64_t)ddx * dx_fixed + (int64_t)ddy * dy_fixed) / 65536);
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
    const int64_t dx_fixed = ((int64_t)x_begin << 16) + 0x8000 - (int64_t)decoded->position.xh;
    const int64_t dy_fixed = (((int64_t)y << 2) + 2 - (int64_t)decoded->position.yh) << 14;

    memset(work, 0, sizeof(*work));
    work->x_begin = x_begin;
    work->x_end = x_end;
    work->y = y;

    if (decoded->has_depth) {
        work->depth_fixed = (int64_t)decoded->depth.z +
                            ((int64_t)decoded->depth.dzdx * dx_fixed +
                             (int64_t)decoded->depth.dzdy * dy_fixed) / 65536;
    }

    if (decoded->has_texture && primitive->color.needs_texel0) {
        work->s_fixed = texture_interpolated_value(decoded->texture.s,
                                                   decoded->texture.dsdx,
                                                   decoded->texture.dsdy,
                                                   dx_fixed,
                                                   dy_fixed);
        work->t_fixed = texture_interpolated_value(decoded->texture.t,
                                                   decoded->texture.dtdx,
                                                   decoded->texture.dtdy,
                                                   dx_fixed,
                                                   dy_fixed);
        if (primitive->texture.perspective) {
            work->w_fixed = texture_interpolated_value(decoded->texture.w,
                                                       decoded->texture.dwdx,
                                                       decoded->texture.dwdy,
                                                       dx_fixed,
                                                       dy_fixed);
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
