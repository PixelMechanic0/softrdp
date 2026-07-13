#include "pipeline.h"

#include "tmem.h"

#include <stddef.h>
#include <string.h>

static uint8_t pipeline_combiner_cycle_mask(const rdp_combiner_cycle *cycle)
{
    uint8_t mask = 0u;
    const uint8_t *sources = (const uint8_t *)cycle;
    for (uint32_t i = 0; i < sizeof(*cycle); i++) {
        switch ((rdp_combiner_source)sources[i]) {
        case RDP_COMBINER_TEXEL0_RGB:
        case RDP_COMBINER_TEXEL0_ALPHA: mask |= RDP_COMBINER_INPUT_TEXEL0; break;
        case RDP_COMBINER_TEXEL1_RGB:
        case RDP_COMBINER_TEXEL1_ALPHA: mask |= RDP_COMBINER_INPUT_TEXEL1; break;
        case RDP_COMBINER_SHADE_RGB:
        case RDP_COMBINER_SHADE_ALPHA: mask |= RDP_COMBINER_INPUT_SHADE; break;
        case RDP_COMBINER_LOD_FRACTION: mask |= RDP_COMBINER_INPUT_LOD_FRACTION; break;
        default: break;
        }
    }
    return mask;
}

static uint16_t compile_depth_delta(const rdp_depth_state *depth,
                                    const raster_decoded_triangle *triangle)
{
    uint32_t value;
    if (depth->source_primitive) {
        value = depth->primitive_delta_z;
    } else {
        const uint32_t dx = (uint32_t)triangle->depth.dzdx >> 16;
        const uint32_t dy = (uint32_t)triangle->depth.dzdy >> 16;
        const uint32_t abs_dx = (dx & 0x8000u) ? (~dx & 0x7fffu) : dx;
        const uint32_t abs_dy = (dy & 0x8000u) ? (~dy & 0x7fffu) : dy;
        value = (abs_dx + abs_dy) & 0xffffu;
        if (value & 0xc000u) return 0x8000u;
        if (value == 0u) return 1u;
        if (value == 1u) return 3u;
        value <<= 1u;
    }
    if (value == 0u) return 0u;
    uint32_t normalized = 1u;
    while (value >>= 1u) normalized <<= 1u;
    return (uint16_t)normalized;
}

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

static void pipeline_compile_texture(rdp_texture_sample_state *texture,
                                     const rdp_state *registers,
                                     const tmem_state *tmem,
                                     uint32_t tile_index,
                                     bool second_cycle)
{
    memset(texture, 0, sizeof(*texture));
    texture->tile_index = (uint8_t)(tile_index & 7u);
    texture->tile = registers->tiles[texture->tile_index];
    texture->perspective = registers->other_modes.perspective;
    texture->tlut_enable = registers->other_modes.tlut_enable;
    texture->tlut_ia = registers->other_modes.tlut_ia;
    texture->bilerp = second_cycle ? registers->other_modes.bilerp1
                                   : registers->other_modes.bilerp0;
    texture->sample_quad = registers->other_modes.sample_quad;
    texture->mid_texel = registers->other_modes.mid_texel;
    texture->convert_one = registers->other_modes.convert_one;
    texture->convert_k0_tf = registers->convert_k0_tf;
    texture->convert_k1_tf = registers->convert_k1_tf;
    texture->convert_k2_tf = registers->convert_k2_tf;
    texture->convert_k3_tf = registers->convert_k3_tf;
    pipeline_resolve_tile_bounds(registers, tmem, texture->tile_index, &texture->bounds);
    uint32_t texture_width;
    uint32_t texture_height;
    uint32_t texture_stride;
    if (tmem_tile_sample_layout(tmem, texture, &texture_width,
                                &texture_height, &texture_stride)) {
        texture->width = (uint16_t)texture_width;
        texture->height = (uint16_t)texture_height;
        texture->stride = (uint16_t)texture_stride;
    }
    if (texture->tile.format == RDP_FORMAT_RGBA &&
        texture->tile.size == RDP_SIZE_16BPP && !texture->tlut_enable) {
        texture->sampler_class = texture->bilerp && texture->sample_quad
            ? RDP_SAMPLER_RGBA16_BILERP : RDP_SAMPLER_RGBA16_POINT;
    } else if (texture->bilerp && texture->sample_quad &&
               texture->tile.format == RDP_FORMAT_I && texture->tile.size == RDP_SIZE_4BPP) {
        texture->sampler_class = RDP_SAMPLER_I4_BILERP;
    } else if (texture->bilerp && texture->sample_quad &&
               texture->tile.format != RDP_FORMAT_YUV &&
               texture->tile.size == RDP_SIZE_8BPP &&
               texture->tlut_enable) {
        texture->sampler_class = RDP_SAMPLER_CI8_TLUT_BILERP;
    } else if (texture->bilerp && texture->sample_quad &&
               texture->tile.format == RDP_FORMAT_I && texture->tile.size == RDP_SIZE_8BPP) {
        texture->sampler_class = RDP_SAMPLER_I8_BILERP;
    } else if (texture->bilerp && texture->sample_quad &&
               texture->tile.format == RDP_FORMAT_IA && texture->tile.size == RDP_SIZE_8BPP) {
        texture->sampler_class = RDP_SAMPLER_IA8_BILERP;
    }
}

static void pipeline_compile_common(rdp_primitive_state *primitive,
                                    const rdp_state *registers,
                                    const tmem_state *tmem,
                                    uint32_t tile_index)
{
    memset(primitive, 0, offsetof(rdp_primitive_state, lod_textures));
    pipeline_compile_framebuffer(&primitive->framebuffer, registers);
    pipeline_compile_texture(&primitive->texture, registers, tmem, tile_index, false);
    primitive->lod_base_tile = primitive->texture.tile_index;
    primitive->lod_min_level = registers->primitive_min_lod & 0x1fu;
    primitive->texture_lod = registers->other_modes.texture_lod;
    primitive->sharpen_lod = registers->other_modes.sharpen_lod;
    primitive->detail_lod = registers->other_modes.detail_lod;
    primitive->fragment.depth.image_address = registers->depth_image_address;
    primitive->fragment.depth.primitive_depth = registers->primitive_depth;
    primitive->fragment.depth.primitive_delta_z = registers->primitive_delta_z;
    primitive->fragment.depth.mode = registers->other_modes.z_mode;
    primitive->fragment.depth.compare = registers->other_modes.z_compare;
    primitive->fragment.depth.update = registers->other_modes.z_update;
    primitive->fragment.depth.source_primitive = registers->other_modes.z_source_primitive;
    primitive->color.program = registers->combiner;
    primitive->color.primitive_color = registers->primitive_color;
    primitive->color.environment_color = registers->environment_color;
    primitive->color.cycle_type = registers->other_modes.cycle_type;
    primitive->color.two_cycle = registers->other_modes.cycle_type == RDP_CYCLE_2;
    primitive->color.primitive_lod_fraction = registers->primitive_lod_fraction;
    uint8_t active_combiner_inputs = pipeline_combiner_cycle_mask(&registers->combiner.cycle[1]);
    if (primitive->color.two_cycle)
        active_combiner_inputs |= pipeline_combiner_cycle_mask(&registers->combiner.cycle[0]);
    primitive->color.needs_texel0 = (active_combiner_inputs & RDP_COMBINER_INPUT_TEXEL0) != 0;
    primitive->color.needs_texel1 = (active_combiner_inputs & RDP_COMBINER_INPUT_TEXEL1) != 0;
    primitive->color.needs_shade = (active_combiner_inputs & RDP_COMBINER_INPUT_SHADE) != 0;
    primitive->color.needs_lod_fraction =
        (active_combiner_inputs & RDP_COMBINER_INPUT_LOD_FRACTION) != 0;
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
    primitive->fragment.blend.input_mask = registers->blender.input_mask[0];
    if (primitive->fragment.blend.cycle_count == 2u)
        primitive->fragment.blend.input_mask |= registers->blender.input_mask[1];
    primitive->fragment.alpha_cvg_select = registers->other_modes.alpha_cvg_select;
    primitive->fragment.cvg_times_alpha = registers->other_modes.cvg_times_alpha;
    primitive->fragment.antialias = registers->other_modes.antialias;
    primitive->fragment.coverage_dest = registers->other_modes.coverage_dest;
    primitive->fragment.rgb_dither = registers->other_modes.rgb_dither;
    primitive->tmem = tmem;
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
    if (has_shade && (primitive->color.needs_shade ||
        (primitive->fragment.blend.input_mask & RDP_BLENDER_INPUT_SHADE_ALPHA)))
        plan->stages |= RDP_BLOCK_STAGE_SHADE;
    if (has_texture && (primitive->color.needs_texel0 || primitive->color.needs_texel1 ||
                        primitive->color.needs_lod_fraction)) {
        plan->stages |= RDP_BLOCK_STAGE_TEXTURE;
        plan->sampler = primitive->texture.sampler_class == RDP_SAMPLER_RGBA16_BILERP
            ? RDP_BLOCK_SAMPLER_RGBA16_BILERP
            : primitive->texture.sampler_class == RDP_SAMPLER_RGBA16_POINT
                ? RDP_BLOCK_SAMPLER_RGBA16_POINT
                : primitive->texture.sampler_class == RDP_SAMPLER_I4_BILERP
                    ? RDP_BLOCK_SAMPLER_I4_BILERP
                    : primitive->texture.sampler_class == RDP_SAMPLER_CI8_TLUT_BILERP
                        ? RDP_BLOCK_SAMPLER_CI8_TLUT_BILERP
                        : primitive->texture.sampler_class == RDP_SAMPLER_I8_BILERP
                            ? RDP_BLOCK_SAMPLER_I8_BILERP
                            : primitive->texture.sampler_class == RDP_SAMPLER_IA8_BILERP
                                ? RDP_BLOCK_SAMPLER_IA8_BILERP : RDP_BLOCK_SAMPLER_GENERIC;
    } else {
        plan->sampler = RDP_BLOCK_SAMPLER_NONE;
    }
    if ((plan->stages & RDP_BLOCK_STAGE_TEXTURE) &&
        (primitive->texture_lod || primitive->sharpen_lod || primitive->detail_lod ||
         primitive->color.needs_lod_fraction))
        plan->stages |= RDP_BLOCK_STAGE_LOD;
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
    primitive->lod_max_level = triangle->position.max_level;
    if (triangle->has_texture && (primitive->texture_lod || primitive->sharpen_lod ||
                                  primitive->detail_lod ||
                                  primitive->color.needs_lod_fraction)) {
        uint32_t tile_count = primitive->texture_lod
            ? (uint32_t)primitive->lod_max_level + 2u : 1u;
        if (tile_count > 8u) tile_count = 8u;
        for (uint32_t offset = 0; offset < tile_count; offset++) {
            const uint32_t tile = (primitive->lod_base_tile + offset) & 7u;
            if (primitive->color.needs_texel0)
                pipeline_compile_texture(&primitive->lod_textures[tile], registers, tmem, tile, false);
            if (primitive->color.needs_texel1)
                pipeline_compile_texture(&primitive->lod_textures_cycle1[tile], registers, tmem, tile, true);
        }
    }
    primitive->fragment.depth.pixel_delta = compile_depth_delta(
        &primitive->fragment.depth, triangle);
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

void pipeline_compile_color_rectangle(rdp_primitive_state *primitive,
                                      const rdp_state *registers,
                                      const tmem_state *tmem)
{
    if (!primitive || !registers) return;
    pipeline_compile_common(primitive, registers, tmem, 0u);
    primitive->span_kernel = RDP_SPAN_KERNEL_TEXTURE_RECTANGLE;
    pipeline_compile_block_plan(primitive, false, false, false);
}
