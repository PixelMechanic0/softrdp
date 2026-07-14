#include "rdp_commands.h"
#include "rdp_memory.h"
#include "raster.h"
#include "tmem.h"
#include "combiner.h"
#include "blender.h"

static rdp_color color_from_word(uint32_t word)
{
    return (rdp_color){ (uint8_t)(word >> 24), (uint8_t)(word >> 16), (uint8_t)(word >> 8), (uint8_t)word };
}

uint8_t rdp_command_word_count(rdp_command_id id)
{
    static const uint8_t lengths[64] = {
        [RDP_CMD_NO_OP] = 2,                          [RDP_CMD_SYNC_LOAD] = 2,
        [RDP_CMD_FILL_TRIANGLE] = 8,                  [RDP_CMD_SYNC_PIPE] = 2,
        [RDP_CMD_FILL_ZBUFFER_TRIANGLE] = 12,         [RDP_CMD_SYNC_TILE] = 2,
        [RDP_CMD_TEXTURE_TRIANGLE] = 24,              [RDP_CMD_SYNC_FULL] = 2,
        [RDP_CMD_TEXTURE_ZBUFFER_TRIANGLE] = 28,      [RDP_CMD_SET_KEY_GB] = 2,
        [RDP_CMD_SHADE_TRIANGLE] = 24,                [RDP_CMD_SET_KEY_R] = 2,
        [RDP_CMD_SHADE_ZBUFFER_TRIANGLE] = 28,        [RDP_CMD_SET_CONVERT] = 2,
        [RDP_CMD_SHADE_TEXTURE_TRIANGLE] = 40,        [RDP_CMD_SET_SCISSOR] = 2,
        [RDP_CMD_SHADE_TEXTURE_ZBUFFER_TRIANGLE] = 44,[RDP_CMD_SET_PRIM_DEPTH] = 2,
        [RDP_CMD_TEXTURE_RECTANGLE] = 4,              [RDP_CMD_SET_OTHER_MODES] = 2,
        [RDP_CMD_TEXTURE_RECTANGLE_FLIP] = 4,         [RDP_CMD_LOAD_TLUT] = 2,
        [RDP_CMD_SET_TILE_SIZE] = 2,                  [RDP_CMD_SET_ENV_COLOR] = 2,
        [RDP_CMD_LOAD_BLOCK] = 2,                     [RDP_CMD_SET_COMBINE] = 2,
        [RDP_CMD_LOAD_TILE] = 2,                      [RDP_CMD_SET_TEXTURE_IMAGE] = 2,
        [RDP_CMD_SET_TILE] = 2,                       [RDP_CMD_SET_MASK_IMAGE] = 2,
        [RDP_CMD_FILL_RECTANGLE] = 2,                 [RDP_CMD_SET_COLOR_IMAGE] = 2,
        [RDP_CMD_SET_FILL_COLOR] = 2,                 [RDP_CMD_SET_FOG_COLOR] = 2,
        [RDP_CMD_SET_BLEND_COLOR] = 2,                [RDP_CMD_SET_PRIM_COLOR] = 2
    };
    return id < 64 ? lengths[id] : 0;
}

bool rdp_command_is_draw(rdp_command_id id)
{
    switch (id) {
    case RDP_CMD_FILL_TRIANGLE:
    case RDP_CMD_FILL_ZBUFFER_TRIANGLE:
    case RDP_CMD_TEXTURE_TRIANGLE:
    case RDP_CMD_TEXTURE_ZBUFFER_TRIANGLE:
    case RDP_CMD_SHADE_TRIANGLE:
    case RDP_CMD_SHADE_ZBUFFER_TRIANGLE:
    case RDP_CMD_SHADE_TEXTURE_TRIANGLE:
    case RDP_CMD_SHADE_TEXTURE_ZBUFFER_TRIANGLE:
    case RDP_CMD_TEXTURE_RECTANGLE:
    case RDP_CMD_TEXTURE_RECTANGLE_FLIP:
    case RDP_CMD_FILL_RECTANGLE:                  return true;
    default:                                      return false;
    }
}

const char *rdp_command_name(rdp_command_id id)
{
    static const char *names[64] = {
        [RDP_CMD_NO_OP] = "NO_OP", [RDP_CMD_FILL_TRIANGLE] = "FILL_TRIANGLE",
        [RDP_CMD_FILL_ZBUFFER_TRIANGLE] = "FILL_ZBUFFER_TRIANGLE", [RDP_CMD_TEXTURE_TRIANGLE] = "TEXTURE_TRIANGLE",
        [RDP_CMD_TEXTURE_ZBUFFER_TRIANGLE] = "TEXTURE_ZBUFFER_TRIANGLE", [RDP_CMD_SHADE_TRIANGLE] = "SHADE_TRIANGLE",
        [RDP_CMD_SHADE_ZBUFFER_TRIANGLE] = "SHADE_ZBUFFER_TRIANGLE", [RDP_CMD_SHADE_TEXTURE_TRIANGLE] = "SHADE_TEXTURE_TRIANGLE",
        [RDP_CMD_SHADE_TEXTURE_ZBUFFER_TRIANGLE] = "SHADE_TEXTURE_ZBUFFER_TRIANGLE", [RDP_CMD_TEXTURE_RECTANGLE] = "TEXTURE_RECTANGLE",
        [RDP_CMD_TEXTURE_RECTANGLE_FLIP] = "TEXTURE_RECTANGLE_FLIP", [RDP_CMD_SYNC_LOAD] = "SYNC_LOAD",
        [RDP_CMD_SYNC_PIPE] = "SYNC_PIPE", [RDP_CMD_SYNC_TILE] = "SYNC_TILE",
        [RDP_CMD_SYNC_FULL] = "SYNC_FULL", [RDP_CMD_SET_KEY_GB] = "SET_KEY_GB",
        [RDP_CMD_SET_KEY_R] = "SET_KEY_R", [RDP_CMD_SET_CONVERT] = "SET_CONVERT",
        [RDP_CMD_SET_SCISSOR] = "SET_SCISSOR", [RDP_CMD_SET_PRIM_DEPTH] = "SET_PRIM_DEPTH",
        [RDP_CMD_SET_OTHER_MODES] = "SET_OTHER_MODES", [RDP_CMD_LOAD_TLUT] = "LOAD_TLUT",
        [RDP_CMD_SET_TILE_SIZE] = "SET_TILE_SIZE", [RDP_CMD_LOAD_BLOCK] = "LOAD_BLOCK",
        [RDP_CMD_LOAD_TILE] = "LOAD_TILE", [RDP_CMD_SET_TILE] = "SET_TILE",
        [RDP_CMD_FILL_RECTANGLE] = "FILL_RECTANGLE", [RDP_CMD_SET_FILL_COLOR] = "SET_FILL_COLOR",
        [RDP_CMD_SET_FOG_COLOR] = "SET_FOG_COLOR", [RDP_CMD_SET_BLEND_COLOR] = "SET_BLEND_COLOR",
        [RDP_CMD_SET_PRIM_COLOR] = "SET_PRIM_COLOR", [RDP_CMD_SET_ENV_COLOR] = "SET_ENV_COLOR",
        [RDP_CMD_SET_COMBINE] = "SET_COMBINE", [RDP_CMD_SET_TEXTURE_IMAGE] = "SET_TEXTURE_IMAGE",
        [RDP_CMD_SET_MASK_IMAGE] = "SET_MASK_IMAGE", [RDP_CMD_SET_COLOR_IMAGE] = "SET_COLOR_IMAGE"
    };
    return id < 64 && names[id] ? names[id] : "UNKNOWN";
}

static void decode_set_image(rdp_image *image, uint32_t w0, uint32_t w1)
{
    image->format = (rdp_texture_format)((w0 >> 21) & 7u);
    image->size = (rdp_texture_size)((w0 >> 19) & 3u);
    image->width = (w0 & 0x3ffu) + 1u;
    image->address = w1 & 0x00ffffffu;
}

static void decode_set_scissor(rdp_set_scissor_cmd *cmd, uint32_t w0, uint32_t w1)
{
    cmd->x0 = (uint16_t)((w0 >> 12) & 0xfffu);
    cmd->y0 = (uint16_t)(w0 & 0xfffu);
    cmd->x1 = (uint16_t)((w1 >> 12) & 0xfffu);
    cmd->y1 = (uint16_t)(w1 & 0xfffu);
}

static void decode_set_other_modes(rdp_other_modes *modes, uint32_t w0, uint32_t w1)
{
    modes->cycle_type = (rdp_cycle_type)((w0 >> 20) & 3u);
    modes->perspective = (w0 & (1u << 19)) != 0;
    modes->detail_lod = (w0 & (1u << 18)) != 0;
    modes->sharpen_lod = (w0 & (1u << 17)) != 0;
    modes->texture_lod = (w0 & (1u << 16)) != 0;
    modes->tlut_enable = (w0 & (1u << 15)) != 0;
    modes->tlut_ia = (w0 & (1u << 14)) != 0;
    modes->sample_quad = (w0 & (1u << 13)) != 0;
    modes->mid_texel = (w0 & (1u << 12)) != 0;
    modes->bilerp0 = (w0 & (1u << 11)) != 0;
    modes->bilerp1 = (w0 & (1u << 10)) != 0;
    modes->convert_one = (w0 & (1u << 9)) != 0;
    modes->rgb_dither = (uint8_t)((w0 >> 6) & 3u);
    modes->alpha_dither = (uint8_t)((w0 >> 4) & 3u);
    modes->force_blend = (w1 & (1u << 14)) != 0;
    modes->alpha_cvg_select = (w1 & (1u << 13)) != 0;
    modes->cvg_times_alpha = (w1 & (1u << 12)) != 0;
    modes->color_on_cvg = (w1 & (1u << 7)) != 0;
    modes->image_read = (w1 & (1u << 6)) != 0;
    modes->z_update = (w1 & (1u << 5)) != 0;
    modes->z_compare = (w1 & (1u << 4)) != 0;
    modes->z_source_primitive = (w1 & (1u << 2)) != 0;
    modes->antialias = (w1 & (1u << 3)) != 0;
    modes->alpha_compare = (w1 & 1u) != 0;
    modes->alpha_compare_dither = (w1 & (1u << 1)) != 0;
    modes->coverage_dest = (uint8_t)((w1 >> 8) & 3u);
    modes->z_mode = (uint8_t)((w1 >> 10) & 3u);
}

static void decode_set_tile(rdp_set_tile_cmd *tile, uint32_t w0, uint32_t w1)
{
    tile->tile_index = (w1 >> 24) & 7u;
    tile->tmem = (uint16_t)((w0 & 0x1ffu) << 3);
    tile->line = (uint16_t)(((w0 >> 9) & 0x1ffu) << 3);
    tile->size = (rdp_texture_size)((w0 >> 19) & 3u);
    tile->format = (rdp_texture_format)((w0 >> 21) & 7u);
    tile->palette = (uint8_t)((w1 >> 20) & 15u);
    tile->shift_t = (uint8_t)((w1 >> 10) & 15u);
    tile->mask_t = (uint8_t)((w1 >> 14) & 15u);
    tile->mirror_t = (uint8_t)((w1 >> 18) & 1u);
    tile->clamp_t = (uint8_t)((w1 >> 19) & 1u);
    tile->shift_s = (uint8_t)(w1 & 15u);
    tile->mask_s = (uint8_t)((w1 >> 4) & 15u);
    tile->mirror_s = (uint8_t)((w1 >> 8) & 1u);
    tile->clamp_s = (uint8_t)((w1 >> 9) & 1u);
}

static void decode_set_tile_size(rdp_set_tile_size_cmd *tile, uint32_t w0, uint32_t w1)
{
    tile->tile_index = (w1 >> 24) & 7u;
    tile->sl = (uint16_t)((w0 >> 12) & 0xfffu);
    tile->tl = (uint16_t)(w0 & 0xfffu);
    tile->sh = (uint16_t)((w1 >> 12) & 0xfffu);
    tile->th = (uint16_t)(w1 & 0xfffu);
}

static void decode_set_combine(rdp_set_combine_cmd *combine, uint32_t w0, uint32_t w1)
{
    rdp_combiner_decode(combine, w0, w1);
}

static void decode_rect(rdp_rect_cmd *rect, const rdp_command *cmd)
{
    rect->xl = (uint16_t)((cmd->words[0] >> 12) & 0xfffu);
    rect->yl = (uint16_t)(cmd->words[0] & 0xfffu);
    rect->x1 = rect->xl >> 2;
    rect->y1 = rect->yl >> 2;
    rect->x0 = ((cmd->words[1] >> 12) & 0xfffu) >> 2;
    rect->y0 = (cmd->words[1] & 0xfffu) >> 2;
    rect->flip = cmd->id == RDP_CMD_TEXTURE_RECTANGLE_FLIP;

    if (cmd->id == RDP_CMD_TEXTURE_RECTANGLE || cmd->id == RDP_CMD_TEXTURE_RECTANGLE_FLIP) {
        rect->tile_index = (cmd->words[1] >> 24) & 7u;
        rect->s0 = (int16_t)(cmd->words[2] >> 16);
        rect->t0 = (int16_t)cmd->words[2];
        /* Preserve the command's five derivative fraction bits. Rectangle
         * scaling must accumulate them before converting to S10.5. */
        rect->dsdx = (int16_t)(cmd->words[3] >> 16);
        rect->dtdy = (int16_t)cmd->words[3];
    } else {
        rect->tile_index = rect->s0 = rect->t0 = rect->dsdx = rect->dtdy = 0;
    }
}

static void decode_load(rdp_load_cmd *load, const rdp_command *cmd)
{
    load->tile_index = (cmd->words[1] >> 24) & 7u;
    load->sl = (uint16_t)((cmd->words[0] >> 12) & 0xfffu);
    load->tl = (uint16_t)(cmd->words[0] & 0xfffu);
    load->sh = (uint16_t)((cmd->words[1] >> 12) & 0xfffu);
    if (cmd->id == RDP_CMD_LOAD_BLOCK) {
        load->dxt = (uint16_t)(cmd->words[1] & 0xfffu);
    } else {
        load->th = (uint16_t)(cmd->words[1] & 0xfffu);
    }
}

sr_result rdp_decode_command(rdp_command *cmd)
{
    if (!cmd) return SR_ERROR_INVALID_ARGUMENT;

    const uint32_t w0 = cmd->words[0];
    const uint32_t w1 = cmd->words[1];

    switch (cmd->id) {
    case RDP_CMD_NO_OP:
    case RDP_CMD_SYNC_LOAD:
    case RDP_CMD_SYNC_PIPE:
    case RDP_CMD_SYNC_TILE:
    case RDP_CMD_SYNC_FULL:
    case RDP_CMD_SET_CONVERT:                     return SR_OK;

    case RDP_CMD_SET_KEY_GB:
        cmd->decoded.set_key.center.g = (uint8_t)(w1 >> 24);
        cmd->decoded.set_key.scale.g = (uint8_t)(w1 >> 16);
        cmd->decoded.set_key.center.b = (uint8_t)(w1 >> 8);
        cmd->decoded.set_key.scale.b = (uint8_t)w1;
        return SR_OK;
    case RDP_CMD_SET_KEY_R:
        cmd->decoded.set_key.center.r = (uint8_t)(w1 >> 8);
        cmd->decoded.set_key.scale.r = (uint8_t)w1;
        return SR_OK;

    case RDP_CMD_SET_COLOR_IMAGE:                 decode_set_image(&cmd->decoded.set_color_image, w0, w1); return SR_OK;
    case RDP_CMD_SET_TEXTURE_IMAGE:               decode_set_image(&cmd->decoded.set_texture_image, w0, w1); return SR_OK;
    case RDP_CMD_SET_MASK_IMAGE:                  cmd->decoded.set_mask_image.address = w1 & 0x00ffffffu; return SR_OK;
    case RDP_CMD_SET_SCISSOR:                     decode_set_scissor(&cmd->decoded.set_scissor, w0, w1); return SR_OK;
    case RDP_CMD_SET_OTHER_MODES:                 decode_set_other_modes(&cmd->decoded.set_other_modes, w0, w1); return SR_OK;
    case RDP_CMD_SET_TILE:                        decode_set_tile(&cmd->decoded.set_tile, w0, w1); return SR_OK;
    case RDP_CMD_SET_TILE_SIZE:                   decode_set_tile_size(&cmd->decoded.set_tile_size, w0, w1); return SR_OK;
    case RDP_CMD_SET_FILL_COLOR:                  cmd->decoded.set_fill_color.fill_color = w1; return SR_OK;
    case RDP_CMD_SET_FOG_COLOR:                   cmd->decoded.set_fog_color.color = color_from_word(w1); return SR_OK;
    case RDP_CMD_SET_BLEND_COLOR:                 cmd->decoded.set_blend_color.color = color_from_word(w1); return SR_OK;
    case RDP_CMD_SET_ENV_COLOR:                   cmd->decoded.set_env_color.color = color_from_word(w1); return SR_OK;
    case RDP_CMD_SET_PRIM_COLOR:                  cmd->decoded.set_prim_color.min_lod = (uint8_t)((w0 >> 8) & 0x1fu); cmd->decoded.set_prim_color.lod_fraction = (uint8_t)(w0 & 0xffu); cmd->decoded.set_prim_color.color = color_from_word(w1); return SR_OK;
    case RDP_CMD_SET_PRIM_DEPTH:                  cmd->decoded.set_prim_depth.depth = (uint16_t)(w1 >> 16); cmd->decoded.set_prim_depth.delta_z = (uint16_t)w1; return SR_OK;
    case RDP_CMD_SET_COMBINE:                     decode_set_combine(&cmd->decoded.set_combine, w0, w1); return SR_OK;

    case RDP_CMD_FILL_TRIANGLE:
    case RDP_CMD_FILL_ZBUFFER_TRIANGLE:
    case RDP_CMD_TEXTURE_TRIANGLE:
    case RDP_CMD_TEXTURE_ZBUFFER_TRIANGLE:
    case RDP_CMD_SHADE_TRIANGLE:
    case RDP_CMD_SHADE_ZBUFFER_TRIANGLE:
    case RDP_CMD_SHADE_TEXTURE_TRIANGLE:
    case RDP_CMD_SHADE_TEXTURE_ZBUFFER_TRIANGLE:  return raster_decode_triangle(cmd, &cmd->decoded.triangle);

    case RDP_CMD_TEXTURE_RECTANGLE:
    case RDP_CMD_TEXTURE_RECTANGLE_FLIP:
    case RDP_CMD_FILL_RECTANGLE:                  decode_rect(&cmd->decoded.rect, cmd); return SR_OK;

    case RDP_CMD_LOAD_TLUT:
    case RDP_CMD_LOAD_BLOCK:
    case RDP_CMD_LOAD_TILE:                       decode_load(&cmd->decoded.load, cmd); return SR_OK;

    default:                                      return SR_ERROR_BAD_COMMAND;
    }
}

sr_result rdp_execute_command(sr_memory *memory,
                              tmem_state *tmem,
                              rdp_state *state,
                              const rdp_command *cmd)
{

    switch (cmd->id) {
    case RDP_CMD_NO_OP:
    case RDP_CMD_SYNC_LOAD:
    case RDP_CMD_SYNC_PIPE:
    case RDP_CMD_SYNC_TILE:
    case RDP_CMD_SYNC_FULL:                       return SR_OK;

    case RDP_CMD_SET_COLOR_IMAGE:
        state->color_image = cmd->decoded.set_color_image;
        state->color_image.address &= (memory->rdram_size - 1u);
        return SR_OK;
    case RDP_CMD_SET_TEXTURE_IMAGE:
        state->texture_image = cmd->decoded.set_texture_image;
        state->texture_image.address &= (memory->rdram_size - 1u);
        return SR_OK;
    case RDP_CMD_SET_MASK_IMAGE:
        state->depth_image_address = cmd->decoded.set_mask_image.address & (memory->rdram_size - 1u);
        return SR_OK;
    case RDP_CMD_SET_SCISSOR:                     state->scissor_x0 = cmd->decoded.set_scissor.x0; state->scissor_y0 = cmd->decoded.set_scissor.y0; state->scissor_x1 = cmd->decoded.set_scissor.x1; state->scissor_y1 = cmd->decoded.set_scissor.y1; return SR_OK;
    case RDP_CMD_SET_OTHER_MODES:
        state->other_modes = cmd->decoded.set_other_modes;
        rdp_blender_decode(&state->blender, cmd->words[1]);
        return SR_OK;

    case RDP_CMD_SET_TILE: {
        const rdp_set_tile_cmd *d = &cmd->decoded.set_tile;
        rdp_tile *t = &state->tiles[d->tile_index];
        t->tmem = d->tmem; t->line = d->line; t->size = d->size; t->format = d->format;
        t->palette = d->palette; t->shift_t = d->shift_t; t->mask_t = d->mask_t;
        t->mirror_t = d->mirror_t; t->clamp_t = d->clamp_t; t->shift_s = d->shift_s;
        t->mask_s = d->mask_s; t->mirror_s = d->mirror_s; t->clamp_s = d->clamp_s;
        return SR_OK;
    }

    case RDP_CMD_SET_TILE_SIZE: {
        const rdp_set_tile_size_cmd *d = &cmd->decoded.set_tile_size;
        rdp_tile *t = &state->tiles[d->tile_index];
        t->sl = d->sl; t->tl = d->tl; t->sh = d->sh; t->th = d->th;
        return SR_OK;
    }

    case RDP_CMD_SET_FILL_COLOR:                  state->fill_color = cmd->decoded.set_fill_color.fill_color; return SR_OK;
    case RDP_CMD_SET_FOG_COLOR:                   state->fog_color = cmd->decoded.set_fog_color.color; return SR_OK;
    case RDP_CMD_SET_BLEND_COLOR:                 state->blend_color = cmd->decoded.set_blend_color.color; return SR_OK;
    case RDP_CMD_SET_ENV_COLOR:                   state->environment_color = cmd->decoded.set_env_color.color; return SR_OK;
    case RDP_CMD_SET_PRIM_COLOR:                  state->primitive_min_lod = cmd->decoded.set_prim_color.min_lod; state->primitive_lod_fraction = cmd->decoded.set_prim_color.lod_fraction; state->primitive_color = cmd->decoded.set_prim_color.color; return SR_OK;
    case RDP_CMD_SET_PRIM_DEPTH:                  state->primitive_depth = cmd->decoded.set_prim_depth.depth; state->primitive_delta_z = cmd->decoded.set_prim_depth.delta_z; return SR_OK;
    case RDP_CMD_SET_KEY_GB:
        state->key_center.g = cmd->decoded.set_key.center.g;
        state->key_scale.g = cmd->decoded.set_key.scale.g;
        state->key_center.b = cmd->decoded.set_key.center.b;
        state->key_scale.b = cmd->decoded.set_key.scale.b;
        return SR_OK;
    case RDP_CMD_SET_KEY_R:
        state->key_center.r = cmd->decoded.set_key.center.r;
        state->key_scale.r = cmd->decoded.set_key.scale.r;
        return SR_OK;

    case RDP_CMD_FILL_TRIANGLE:
    case RDP_CMD_FILL_ZBUFFER_TRIANGLE:
    case RDP_CMD_TEXTURE_TRIANGLE:
    case RDP_CMD_TEXTURE_ZBUFFER_TRIANGLE:
    case RDP_CMD_SHADE_TRIANGLE:
    case RDP_CMD_SHADE_ZBUFFER_TRIANGLE:
    case RDP_CMD_SHADE_TEXTURE_TRIANGLE:
    case RDP_CMD_SHADE_TEXTURE_ZBUFFER_TRIANGLE:  return raster_submit_triangle(memory, tmem, state, cmd);

    case RDP_CMD_TEXTURE_RECTANGLE:
    case RDP_CMD_TEXTURE_RECTANGLE_FLIP:
    case RDP_CMD_FILL_RECTANGLE:                  return raster_submit_rectangle(memory, tmem, state, cmd);

    case RDP_CMD_LOAD_BLOCK: {
        rdp_tile *tile = &state->tiles[cmd->decoded.load.tile_index];
        tile->sl = cmd->decoded.load.sl;
        tile->tl = cmd->decoded.load.tl;
        tile->sh = cmd->decoded.load.sh;
        tile->th = cmd->decoded.load.dxt;
        return tmem_load_tile(tmem, memory, state, cmd);
    }

    case RDP_CMD_LOAD_TLUT:
    case RDP_CMD_LOAD_TILE:                       return tmem_load_tile(tmem, memory, state, cmd);

    case RDP_CMD_SET_COMBINE:                    state->combiner = cmd->decoded.set_combine; return SR_OK;

    case RDP_CMD_SET_CONVERT: {
        int32_t k0 = (int32_t)((cmd->words[0] >> 13) & 0x1ffu);
        int32_t k1 = (int32_t)((cmd->words[0] >> 4) & 0x1ffu);
        int32_t k2 = (int32_t)(((cmd->words[0] & 0xfu) << 5) | ((cmd->words[1] >> 27) & 0x1fu));
        int32_t k3 = (int32_t)((cmd->words[1] >> 18) & 0x1ffu);
        
        // Sign-extend 9-bit values
        k0 = (k0 & 0x100) ? (k0 | ~0x1ff) : k0;
        k1 = (k1 & 0x100) ? (k1 | ~0x1ff) : k1;
        k2 = (k2 & 0x100) ? (k2 | ~0x1ff) : k2;
        k3 = (k3 & 0x100) ? (k3 | ~0x1ff) : k3;

        state->convert_k0_tf = (k0 << 1) + 1;
        state->convert_k1_tf = (k1 << 1) + 1;
        state->convert_k2_tf = (k2 << 1) + 1;
        state->convert_k3_tf = (k3 << 1) + 1;
        state->convert_k4 = (int32_t)((cmd->words[1] >> 9) & 0x1ffu);
        state->convert_k5 = (int32_t)(cmd->words[1] & 0x1ffu);
        return SR_OK;
    }

    default:                                      return SR_ERROR_BAD_COMMAND;
    }
}
