#include "pipeline.h"

#include "tmem.h"

#include <string.h>

void pipeline_resolve_tile_bounds(const rdp_state *state,
                                  const tmem_state *tmem,
                                  uint32_t tile_index,
                                  rdp_tile_bounds *bounds)
{
    if (!bounds) {
        return;
    }
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
    primitive->span_kernel = pipeline_select_triangle_kernel(primitive);
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
    primitive->span_kernel = RDP_SPAN_KERNEL_TEXTURE_RECTANGLE;
}
