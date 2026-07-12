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
    primitive->texture.tlut_ia = registers->other_modes.tlut_ia;
    primitive->texture.bilerp = registers->other_modes.bilerp0;
    primitive->texture.sample_quad = registers->other_modes.sample_quad;
    primitive->texture.mid_texel = registers->other_modes.mid_texel;
    primitive->texture.convert_one = registers->other_modes.convert_one;
    primitive->texture.convert_k0_tf = registers->convert_k0_tf;
    primitive->texture.convert_k1_tf = registers->convert_k1_tf;
    primitive->texture.convert_k2_tf = registers->convert_k2_tf;
    primitive->texture.convert_k3_tf = registers->convert_k3_tf;
    primitive->fragment.depth.image_address = registers->depth_image_address;
    primitive->fragment.depth.primitive_depth = registers->primitive_depth;
    primitive->fragment.depth.compare = registers->other_modes.z_compare;
    primitive->fragment.depth.update = registers->other_modes.z_update;
    primitive->fragment.depth.source_primitive = registers->other_modes.z_source_primitive;
    primitive->color.program = registers->combiner;
    primitive->color.primitive_color = registers->primitive_color;
    primitive->color.environment_color = registers->environment_color;
    primitive->color.cycle_type = registers->other_modes.cycle_type;
    primitive->color.primitive_lod_fraction = registers->primitive_lod_fraction;
    primitive->color.needs_texel0 = (registers->combiner.input_mask & RDP_COMBINER_INPUT_TEXEL0) != 0;
    primitive->color.needs_texel1 = (registers->combiner.input_mask & RDP_COMBINER_INPUT_TEXEL1) != 0;
    primitive->color.needs_shade = (registers->combiner.input_mask & RDP_COMBINER_INPUT_SHADE) != 0;
    primitive->color.convert_k4 = registers->convert_k4;
    primitive->color.convert_k5 = registers->convert_k5;
    primitive->fragment.blend.program = registers->blender;
    primitive->fragment.blend.fog_color = registers->fog_color;
    primitive->fragment.blend.blend_color = registers->blend_color;
    primitive->fragment.blend.cycle_type = registers->other_modes.cycle_type;
    primitive->fragment.blend.force_blend = registers->other_modes.force_blend;
    primitive->fragment.blend.image_read = registers->other_modes.image_read;
    primitive->fragment.blend.alpha_compare = registers->other_modes.alpha_compare;
    primitive->fragment.alpha_cvg_select = registers->other_modes.alpha_cvg_select;
    primitive->fragment.cvg_times_alpha = registers->other_modes.cvg_times_alpha;
    primitive->fragment.antialias = registers->other_modes.antialias;
    primitive->fragment.coverage_dest = registers->other_modes.coverage_dest;
    primitive->tmem = tmem;
    primitive->metrics = metrics;
    pipeline_resolve_tile_bounds(registers,
                                 tmem,
                                 primitive->texture.tile_index,
                                 &primitive->texture.bounds);
    uint32_t texture_width;
    uint32_t texture_height;
    uint32_t texture_stride;
    if (tmem_tile_sample_layout(tmem,
                                &primitive->texture,
                                &texture_width,
                                &texture_height,
                                &texture_stride)) {
        primitive->texture.width = (uint16_t)texture_width;
        primitive->texture.height = (uint16_t)texture_height;
        primitive->texture.stride = (uint16_t)texture_stride;
    }
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
    primitive->span_kernel = RDP_SPAN_KERNEL_TRIANGLE;
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
    primitive->span_kernel = registers->other_modes.cycle_type == RDP_CYCLE_COPY
        ? RDP_SPAN_KERNEL_TEXTURE_RECTANGLE_COPY
        : RDP_SPAN_KERNEL_TEXTURE_RECTANGLE;
}
