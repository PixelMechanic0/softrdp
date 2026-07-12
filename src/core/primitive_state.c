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
    framebuffer->bytes_per_pixel = registers->color_image.size == RDP_SIZE_32BPP ? 4u :
                                   registers->color_image.size == RDP_SIZE_16BPP ? 2u : 1u;
}

static void pipeline_compile_common(rdp_primitive_state *primitive,
                                    const rdp_state *registers,
                                    const tmem_state *tmem,
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
    primitive->color.two_cycle = registers->other_modes.cycle_type == RDP_CYCLE_2;
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
    primitive->fragment.blend.cycle_count = registers->other_modes.cycle_type == RDP_CYCLE_2 ? 2u : 1u;
    primitive->fragment.blend.final_cycle = registers->other_modes.cycle_type == RDP_CYCLE_2 ? 1u : 0u;
    primitive->fragment.alpha_cvg_select = registers->other_modes.alpha_cvg_select;
    primitive->fragment.cvg_times_alpha = registers->other_modes.cvg_times_alpha;
    primitive->fragment.antialias = registers->other_modes.antialias;
    primitive->fragment.coverage_dest = registers->other_modes.coverage_dest;
    primitive->tmem = tmem;
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
    if (primitive->texture.tile.format == RDP_FORMAT_RGBA &&
        primitive->texture.tile.size == RDP_SIZE_16BPP &&
        !primitive->texture.tlut_enable) {
        primitive->texture.sampler_class = primitive->texture.bilerp && primitive->texture.sample_quad
            ? 2u : 1u;
    }
}

static void pipeline_compile_block_plan(rdp_primitive_state *primitive,
                                        bool has_texture,
                                        bool has_shade,
                                        bool has_depth)
{
    rdp_block_plan *plan = &primitive->block_plan;
    const rdp_depth_state *depth = &primitive->fragment.depth;
    plan->stages = 0u;
    plan->coordinates = primitive->texture.perspective
        ? RDP_BLOCK_COORD_PERSPECTIVE : RDP_BLOCK_COORD_DIRECT;
    plan->framebuffer = primitive->framebuffer.color_image.size == RDP_SIZE_32BPP
        ? RDP_BLOCK_FRAMEBUFFER_32
        : primitive->framebuffer.color_image.size == RDP_SIZE_16BPP
            ? RDP_BLOCK_FRAMEBUFFER_16 : RDP_BLOCK_FRAMEBUFFER_8;
    plan->depth = depth->compare
        ? (depth->update ? RDP_BLOCK_DEPTH_TEST_UPDATE : RDP_BLOCK_DEPTH_TEST)
        : (depth->update ? RDP_BLOCK_DEPTH_UPDATE : RDP_BLOCK_DEPTH_NONE);
    if (plan->depth != RDP_BLOCK_DEPTH_NONE && depth->image_address &&
        (has_depth || depth->source_primitive))
        plan->stages |= RDP_BLOCK_STAGE_DEPTH;
    if (has_shade && primitive->color.needs_shade)
        plan->stages |= RDP_BLOCK_STAGE_SHADE;
    if (has_texture && (primitive->color.needs_texel0 || primitive->color.needs_texel1)) {
        plan->stages |= RDP_BLOCK_STAGE_TEXTURE;
        plan->sampler = primitive->texture.sampler_class == 2u
            ? RDP_BLOCK_SAMPLER_RGBA16_BILERP
            : primitive->texture.sampler_class == 1u
                ? RDP_BLOCK_SAMPLER_RGBA16_POINT : RDP_BLOCK_SAMPLER_GENERIC;
    } else {
        plan->sampler = RDP_BLOCK_SAMPLER_NONE;
    }
    if (primitive->fragment.blend.force_blend || primitive->fragment.blend.image_read)
        plan->stages |= RDP_BLOCK_STAGE_BLEND;
    if (primitive->fragment.blend.alpha_compare)
        plan->stages |= RDP_BLOCK_STAGE_ALPHA_COMPARE;
    if (primitive->fill_mode)
        plan->stages |= RDP_BLOCK_STAGE_FILL;
}

void pipeline_compile_triangle(rdp_primitive_state *primitive,
                               const rdp_state *registers,
                               const tmem_state *tmem,
                               const raster_decoded_triangle *triangle,
                               bool fill_mode)
{
    if (!primitive || !registers || !triangle) {
        return;
    }

    pipeline_compile_common(primitive,
                            registers,
                            tmem,
                            triangle->position.tile);
    primitive->triangle = *triangle;
    primitive->fill_mode = fill_mode;
    primitive->span_kernel = RDP_SPAN_KERNEL_TRIANGLE;
    pipeline_compile_block_plan(primitive, triangle->has_texture,
                                triangle->has_shade, triangle->has_depth);
}

void pipeline_compile_rectangle(rdp_primitive_state *primitive,
                                const rdp_state *registers,
                                const tmem_state *tmem,
                                uint32_t tile_index)
{
    if (!primitive || !registers) {
        return;
    }

    pipeline_compile_common(primitive, registers, tmem, tile_index);
    primitive->span_kernel = registers->other_modes.cycle_type == RDP_CYCLE_COPY
        ? RDP_SPAN_KERNEL_TEXTURE_RECTANGLE_COPY
        : RDP_SPAN_KERNEL_TEXTURE_RECTANGLE;
    pipeline_compile_block_plan(primitive, true, false, false);
}
