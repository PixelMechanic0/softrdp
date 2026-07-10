#include "pipeline.h"
#include <string.h>
#include "framebuffer.h"
#include "rdp_memory.h"
#include "tmem.h"

static uint8_t multiply_u8(uint8_t a, uint8_t b)
{
    return (uint8_t)(((uint32_t)a * (uint32_t)b + 127u) / 255u);
}

static rdp_color modulate_color(rdp_color texel, rdp_color shade)
{
    return (rdp_color){
        multiply_u8(texel.r, shade.r),
        multiply_u8(texel.g, shade.g),
        multiply_u8(texel.b, shade.b),
        multiply_u8(texel.a, shade.a)
    };
}

pipeline_outputs pipeline_shade_pixel(const rdp_color_pipeline_state *state, const pipeline_inputs *inputs)
{
    pipeline_outputs out = { .coverage = 7 };
    if (!inputs) out.color = (rdp_color){0, 0, 0, 255};
    else if (state && state->combiner == RDP_SIMPLE_COMBINER_PRIMITIVE) out.color = inputs->primitive;
    else if (state && state->combiner == RDP_SIMPLE_COMBINER_TEXEL0_SHADE) out.color = modulate_color(inputs->texel0, inputs->shade);
    else out.color = inputs->texel0;
    return out;
}

/* Moved Fragment Processing Helper Functions */

static uint8_t shade_component_to_u8(int32_t value)
{
    int32_t component = value >> 16;
    return component < 0 ? 0 : (component > 255 ? 255 : (uint8_t)component);
}

static rdp_color shade_base_color(const raster_shade_setup *shade)
{
    return (rdp_color){
        shade_component_to_u8(shade->r),
        shade_component_to_u8(shade->g),
        shade_component_to_u8(shade->b),
        shade_component_to_u8(shade->a)
    };
}

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
    if (value < INT32_MIN) return INT32_MIN;
    if (value > INT32_MAX) return INT32_MAX;
    return (int32_t)value;
}

static int32_t texture_interpolated_value(int32_t base, int32_t ddx, int32_t ddy, int64_t dx_fixed, int64_t dy_fixed)
{
    return (int32_t)((int64_t)base + ((int64_t)ddx * dx_fixed + (int64_t)ddy * dy_fixed) / 65536);
}

static int32_t perspective_divide_coord(int32_t coord, int32_t w)
{
    if (w <= 0) return 0;
    int64_t divided = ((int64_t)coord << 16) / (int64_t)w;
    return divided < 0 ? 0 : (divided > INT32_MAX ? INT32_MAX : (int32_t)divided);
}

static uint32_t texture_coord_to_texel(int32_t value)
{
    return value <= 0 ? 0 : (uint32_t)value >> 5;
}

static uint16_t depth_fixed_to_u16(int64_t value)
{
    return value <= 0 ? 0 : (value >= 0xffff0000ll ? 0xffffu : (uint16_t)((uint64_t)value >> 16));
}

static void record_texture_sample_coord(rdp_metrics *metrics, uint32_t s, uint32_t t)
{
    if (!metrics) {
        return;
    }

    if (metrics->texture_sample_attempts == 0) {
        metrics->texture_sample_min_s = s;
        metrics->texture_sample_max_s = s;
        metrics->texture_sample_min_t = t;
        metrics->texture_sample_max_t = t;
    } else {
        if (s < metrics->texture_sample_min_s) metrics->texture_sample_min_s = s;
        if (s > metrics->texture_sample_max_s) metrics->texture_sample_max_s = s;
        if (t < metrics->texture_sample_min_t) metrics->texture_sample_min_t = t;
        if (t > metrics->texture_sample_max_t) metrics->texture_sample_max_t = t;
    }
}

static void record_texture_sample_fixed(rdp_metrics *metrics, int32_t s, int32_t t, int32_t w)
{
    if (!metrics) {
        return;
    }

    if (metrics->texture_sample_attempts == 0) {
        metrics->texture_sample_min_s_fixed = s;
        metrics->texture_sample_max_s_fixed = s;
        metrics->texture_sample_min_t_fixed = t;
        metrics->texture_sample_max_t_fixed = t;
        metrics->texture_sample_min_w_fixed = w;
        metrics->texture_sample_max_w_fixed = w;
    } else {
        if (s < metrics->texture_sample_min_s_fixed) metrics->texture_sample_min_s_fixed = s;
        if (s > metrics->texture_sample_max_s_fixed) metrics->texture_sample_max_s_fixed = s;
        if (t < metrics->texture_sample_min_t_fixed) metrics->texture_sample_min_t_fixed = t;
        if (t > metrics->texture_sample_max_t_fixed) metrics->texture_sample_max_t_fixed = t;
        if (w < metrics->texture_sample_min_w_fixed) metrics->texture_sample_min_w_fixed = w;
        if (w > metrics->texture_sample_max_w_fixed) metrics->texture_sample_max_w_fixed = w;
    }
}

static void record_texture_sample_color(rdp_metrics *metrics, rdp_color color)
{
    if (!metrics) {
        return;
    }

    const uint32_t packed = ((uint32_t)color.r << 24) |
                            ((uint32_t)color.g << 16) |
                            ((uint32_t)color.b << 8) |
                            (uint32_t)color.a;
    metrics->texture_sample_color_xor ^= packed;
}

static void record_texture_sample_attempt(rdp_metrics *metrics,
                                          const rdp_texture_sample_state *texture,
                                          const rdp_color_pipeline_state *color)
{
    if (!metrics || !texture || !color) {
        return;
    }

    const rdp_tile *tile = &texture->tile;
    metrics->texture_sample_attempts++;
    if (tile->format <= RDP_FORMAT_I && tile->size <= RDP_SIZE_32BPP) {
        metrics->texture_sample_by_format_size[tile->format][tile->size]++;
    }
    if (texture->tlut_enable) {
        metrics->texture_sample_tlut_attempts++;
    }
    if (texture->bilerp) {
        metrics->texture_sample_bilerp_attempts++;
    }
    if (texture->sample_quad) {
        metrics->texture_sample_quad_attempts++;
    }
    if (texture->mid_texel) {
        metrics->texture_sample_mid_texel_attempts++;
    }
    if (texture->perspective) {
        metrics->texture_sample_perspective_attempts++;
    }
    if (color->combiner == RDP_SIMPLE_COMBINER_TEXEL0_SHADE) {
        metrics->texture_sample_texel0_shade_attempts++;
    }
}

static void record_texture_sample_hit(rdp_metrics *metrics, const rdp_tile *tile)
{
    if (!metrics || !tile) {
        return;
    }

    metrics->texture_sample_hits++;
    if (tile->format <= RDP_FORMAT_I && tile->size <= RDP_SIZE_32BPP) {
        metrics->texture_sample_hits_by_format_size[tile->format][tile->size]++;
    }
}

void pipeline_compile_framebuffer(rdp_framebuffer_state *framebuffer,
                                  const rdp_state *registers)
{
    if (!framebuffer || !registers) {
        return;
    }

    framebuffer->color_image = registers->color_image;
    framebuffer->fill_color = registers->fill_color;
}

static void pipeline_compile_common(rdp_primitive_state *primitive,
                                    const rdp_state *registers,
                                    const tmem_state *tmem,
                                    rdp_metrics *metrics,
                                    uint32_t tile_index)
{
    memset(primitive, 0, sizeof(*primitive));
    pipeline_compile_framebuffer(&primitive->framebuffer, registers);
    primitive->texture.tile_index = (uint8_t)(tile_index & 7u);
    primitive->texture.tile = registers->tiles[primitive->texture.tile_index];
    primitive->texture.perspective = registers->other_modes.perspective;
    primitive->texture.tlut_enable = registers->other_modes.tlut_enable;
    primitive->texture.bilerp = registers->other_modes.bilerp0;
    primitive->texture.sample_quad = registers->other_modes.sample_quad;
    primitive->texture.mid_texel = registers->other_modes.mid_texel;
    primitive->depth.image_address = registers->depth_image_address;
    primitive->depth.compare = registers->other_modes.z_compare;
    primitive->depth.update = registers->other_modes.z_update;
    primitive->color.combiner = registers->simple_combiner;
    primitive->color.primitive_color = registers->primitive_color;
    primitive->color.needs_texel0 = registers->combiner_needs_texel0;
    primitive->color.needs_shade = registers->combiner_needs_shade;
    primitive->tmem = tmem;
    primitive->metrics = metrics;
    pipeline_resolve_tile_bounds(registers,
                                 tmem,
                                 primitive->texture.tile_index,
                                 &primitive->texture.bounds);
}

void pipeline_compile_triangle(rdp_primitive_state *primitive,
                               const rdp_state *registers,
                               const tmem_state *tmem,
                               rdp_metrics *metrics,
                               const raster_decoded_triangle *triangle,
                               bool fill_mode)
{
    if (!primitive || !registers || !triangle) {
        return;
    }

    pipeline_compile_common(primitive,
                            registers,
                            tmem,
                            metrics,
                            triangle->position.tile);
    primitive->triangle = *triangle;
    primitive->fill_mode = fill_mode;
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
    const rdp_color_pipeline_state *color = &primitive->color;
    const rdp_texture_sample_state *texture = &primitive->texture;
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

    if (decoded->has_texture && color->needs_texel0) {
        work->s_fixed = texture_interpolated_value(decoded->texture.s, decoded->texture.dsdx, decoded->texture.dsdy, dx_fixed, dy_fixed);
        work->t_fixed = texture_interpolated_value(decoded->texture.t, decoded->texture.dtdx, decoded->texture.dtdy, dx_fixed, dy_fixed);
        if (texture->perspective) {
            work->w_fixed = texture_interpolated_value(decoded->texture.w, decoded->texture.dwdx, decoded->texture.dwdy, dx_fixed, dy_fixed);
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

static void triangle_span_work_step(const raster_decoded_triangle *decoded,
                                    const rdp_primitive_state *primitive,
                                    rdp_span_work *work)
{
    if (decoded->has_depth) {
        work->depth_fixed += decoded->depth.dzdx;
    }
    if (decoded->has_texture && primitive->color.needs_texel0) {
        work->s_fixed += decoded->texture.dsdx;
        work->t_fixed += decoded->texture.dtdx;
        if (primitive->texture.perspective) {
            work->w_fixed += decoded->texture.dwdx;
        }
    }
    if (decoded->has_shade) {
        work->shade.r += decoded->shade.drdx & ~0x1f;
        work->shade.g += decoded->shade.dgdx & ~0x1f;
        work->shade.b += decoded->shade.dbdx & ~0x1f;
        work->shade.a += decoded->shade.dadx & ~0x1f;
    }
}

/*
 * Span processing is the intended long-term raster pipeline shape, not a
 * one-off optimization. Rasterization should produce spans; pipeline stages
 * should consume spans with incremental shade/texture/depth state. As coverage,
 * blending, LOD, fog, and cycle modes grow, split this into explicit span setup,
 * texture, combine, depth/blend, and framebuffer stages rather than returning
 * to per-pixel full interpolation.
 */
sr_result pipeline_render_triangle_span(sr_memory *memory,
                                        const rdp_primitive_state *primitive,
                                        const rdp_span_work *work)
{
    if (!memory || !primitive || !work) {
        return SR_ERROR_INVALID_ARGUMENT;
    }
    if (work->x_end < work->x_begin) {
        return SR_OK;
    }

    const raster_decoded_triangle *decoded = &primitive->triangle;
    const rdp_framebuffer_state *framebuffer = &primitive->framebuffer;
    const rdp_texture_sample_state *texture = &primitive->texture;
    const rdp_depth_state *depth = &primitive->depth;
    const rdp_color_pipeline_state *color_pipeline = &primitive->color;
    const tmem_state *tmem = primitive->tmem;
    rdp_metrics *metrics = primitive->metrics;
    rdp_span_work cursor = *work;

    for (int x = cursor.x_begin; x <= cursor.x_end; x++) {
        bool visible = true;
        if (decoded->has_depth && depth->image_address != 0) {
            uint16_t old_depth = 0xffffu;
            const uint16_t new_depth = depth_fixed_to_u16(cursor.depth_fixed);
            const uint32_t pixel = (uint32_t)cursor.y * framebuffer->color_image.width + (uint32_t)x;
            const uint32_t addr = depth->image_address + pixel * 2u;

            if ((depth->compare || depth->update) &&
                !sr_memory_read_be16(memory, addr, &old_depth)) {
                return SR_ERROR_INVALID_ARGUMENT;
            }
            if (depth->compare && new_depth > old_depth) {
                visible = false;
            } else if (depth->update &&
                       !sr_memory_write_be16(memory, addr, new_depth)) {
                return SR_ERROR_INVALID_ARGUMENT;
            }
        }

        if (visible) {
            if (primitive->fill_mode) {
                sr_result result = framebuffer_write_fill_pixel(memory, framebuffer, (uint32_t)x, (uint32_t)cursor.y);
                if (result != SR_OK) {
                    return result;
                }
            } else if (decoded->has_texture) {
                const bool needs_texel0 = color_pipeline->needs_texel0;
                bool wrote_color = false;
                if (needs_texel0) {
                    int32_t s_fixed = cursor.s_fixed;
                    int32_t t_fixed = cursor.t_fixed;
                    if (texture->perspective) {
                        s_fixed = perspective_divide_coord(s_fixed, cursor.w_fixed);
                        t_fixed = perspective_divide_coord(t_fixed, cursor.w_fixed);
                    }

                    rdp_color texel0;
                    record_texture_sample_fixed(metrics, s_fixed, t_fixed, texture->perspective ? cursor.w_fixed : 0);
                    record_texture_sample_coord(metrics, texture_coord_to_texel(s_fixed), texture_coord_to_texel(t_fixed));
                    record_texture_sample_attempt(metrics, texture, color_pipeline);
                    if (tmem_sample_color_fixed5(tmem, texture, s_fixed, t_fixed, &texel0)) {
                        record_texture_sample_hit(metrics, &texture->tile);
                        record_texture_sample_color(metrics, texel0);
                        rdp_color color;
                        if (decoded->has_shade && color_pipeline->needs_shade) {
                            const rdp_color shade = shade_base_color(&cursor.shade);
                            color = modulate_color(texel0, shade);
                        } else {
                            color = pipeline_shade_pixel(color_pipeline, &(pipeline_inputs){
                                .texel0 = texel0,
                                .primitive = color_pipeline->primitive_color
                            }).color;
                        }
                        sr_result result = framebuffer_write_color(memory, framebuffer, (uint32_t)x, (uint32_t)cursor.y, color);
                        if (result != SR_OK) {
                            return result;
                        }
                        wrote_color = true;
                    } else {
                        if (metrics) {
                            metrics->texture_sample_misses++;
                        }
                    }
                } else {
                    sr_result result = framebuffer_write_color(memory, framebuffer, (uint32_t)x, (uint32_t)cursor.y,
                                                               color_pipeline->primitive_color);
                    if (result != SR_OK) {
                        return result;
                    }
                    wrote_color = true;
                }

                if (!wrote_color && decoded->has_shade) {
                    if (metrics) {
                        metrics->texture_sample_shade_fallbacks++;
                    }
                    sr_result result = framebuffer_write_color(memory, framebuffer, (uint32_t)x, (uint32_t)cursor.y,
                                                               shade_base_color(&cursor.shade));
                    if (result != SR_OK) {
                        return result;
                    }
                }
            } else if (decoded->has_shade) {
                sr_result result = framebuffer_write_color(memory, framebuffer, (uint32_t)x, (uint32_t)cursor.y,
                                                           shade_base_color(&cursor.shade));
                if (result != SR_OK) {
                    return result;
                }
            }
        }

        triangle_span_work_step(decoded, primitive, &cursor);
    }

    return SR_OK;
}

static sr_result render_rectangle_pixel(sr_memory *memory,
                                        const tmem_state *tmem,
                                        const rdp_framebuffer_state *framebuffer,
                                        const rdp_texture_sample_state *texture,
                                        const rdp_color_pipeline_state *color_pipeline,
                                        rdp_metrics *metrics,
                                        uint32_t s,
                                        uint32_t t,
                                        uint32_t x,
                                        uint32_t y)
{
    if (color_pipeline->combiner == RDP_SIMPLE_COMBINER_PRIMITIVE) {
        return framebuffer_write_color(memory, framebuffer, x, y, color_pipeline->primitive_color);
    }

    rdp_color texel0;
    if (metrics) {
        metrics->rect_texture_sample_attempts++;
    }
    if (!tmem_sample_color(tmem, texture, s, t, &texel0)) {
        if (metrics) {
            metrics->rect_texture_sample_misses++;
        }
        return SR_ERROR_INVALID_ARGUMENT;
    }
    if (metrics) {
        metrics->rect_texture_sample_hits++;
    }

    pipeline_inputs inputs = {
        .texel0 = texel0,
        .primitive = color_pipeline->primitive_color
    };
    pipeline_outputs outputs = pipeline_shade_pixel(color_pipeline, &inputs);
    return framebuffer_write_color(memory, framebuffer, x, y, outputs.color);
}

void pipeline_compile_rectangle(rdp_primitive_state *primitive,
                                const rdp_state *registers,
                                const tmem_state *tmem,
                                rdp_metrics *metrics,
                                uint32_t tile_index)
{
    if (!primitive || !registers) {
        return;
    }

    pipeline_compile_common(primitive, registers, tmem, metrics, tile_index);
}

void pipeline_setup_rectangle_span(int x_begin,
                                   int x_end,
                                   int y,
                                   int32_t s_fixed,
                                   int32_t t_fixed,
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
}

sr_result pipeline_render_rectangle_span(sr_memory *memory,
                                         const rdp_primitive_state *primitive,
                                         const rdp_span_work *work,
                                         int32_t dsdx,
                                         int32_t dtdx)
{
    if (!memory || !primitive || !work) {
        return SR_ERROR_INVALID_ARGUMENT;
    }

    int32_t s_fixed = work->s_fixed;
    int32_t t_fixed = work->t_fixed;
    for (int x = work->x_begin; x <= work->x_end; x++) {
        const uint32_t s = s_fixed < 0 ? 0u : (uint32_t)s_fixed >> 5;
        const uint32_t t = t_fixed < 0 ? 0u : (uint32_t)t_fixed >> 5;
        sr_result result = render_rectangle_pixel(memory,
                                                  primitive->tmem,
                                                  &primitive->framebuffer,
                                                  &primitive->texture,
                                                  &primitive->color,
                                                  primitive->metrics,
                                                  s,
                                                  t,
                                                  (uint32_t)x,
                                                  (uint32_t)work->y);
        if (result != SR_OK) {
            return result;
        }
        s_fixed += dsdx;
        t_fixed += dtdx;
    }

    return SR_OK;
}

void pipeline_resolve_tile_bounds(const rdp_state *state, const tmem_state *tmem, uint32_t tile_index, rdp_tile_bounds *bounds)
{
    if (!bounds) return;
    if (!state || !tmem || tile_index >= 8) {
        memset(bounds, 0, sizeof(*bounds));
        return;
    }
    const rdp_tile *tile = &state->tiles[tile_index];
    bounds->sl = tmem->tile_sl[tile_index];
    bounds->tl = tmem->tile_tl[tile_index];
    bounds->sh = tmem->tile_sh[tile_index];
    bounds->th = tmem->tile_th[tile_index];
    if (tile->sh > tile->sl || tile->th > tile->tl) {
        bounds->sl = tile->sl >> 2;
        bounds->tl = tile->tl >> 2;
        bounds->sh = tile->sh >> 2;
        bounds->th = tile->th >> 2;
    }
}
