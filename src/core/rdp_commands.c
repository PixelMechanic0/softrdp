#include "rdp_commands.h"

#include "raster.h"
#include "tmem.h"

static rdp_color color_from_word(uint32_t word)
{
    rdp_color color;
    color.r = (uint8_t)(word >> 24);
    color.g = (uint8_t)(word >> 16);
    color.b = (uint8_t)(word >> 8);
    color.a = (uint8_t)word;
    return color;
}

uint8_t rdp_command_word_count(rdp_command_id id)
{
    static const uint8_t lengths[64] = {
        [RDP_CMD_NO_OP] = 2,
        [RDP_CMD_FILL_TRIANGLE] = 8,
        [RDP_CMD_FILL_ZBUFFER_TRIANGLE] = 12,
        [RDP_CMD_TEXTURE_TRIANGLE] = 24,
        [RDP_CMD_TEXTURE_ZBUFFER_TRIANGLE] = 28,
        [RDP_CMD_SHADE_TRIANGLE] = 24,
        [RDP_CMD_SHADE_ZBUFFER_TRIANGLE] = 28,
        [RDP_CMD_SHADE_TEXTURE_TRIANGLE] = 40,
        [RDP_CMD_SHADE_TEXTURE_ZBUFFER_TRIANGLE] = 44,
        [RDP_CMD_TEXTURE_RECTANGLE] = 4,
        [RDP_CMD_TEXTURE_RECTANGLE_FLIP] = 4,
        [RDP_CMD_SYNC_LOAD] = 2,
        [RDP_CMD_SYNC_PIPE] = 2,
        [RDP_CMD_SYNC_TILE] = 2,
        [RDP_CMD_SYNC_FULL] = 2,
        [RDP_CMD_SET_KEY_GB] = 2,
        [RDP_CMD_SET_KEY_R] = 2,
        [RDP_CMD_SET_CONVERT] = 2,
        [RDP_CMD_SET_SCISSOR] = 2,
        [RDP_CMD_SET_PRIM_DEPTH] = 2,
        [RDP_CMD_SET_OTHER_MODES] = 2,
        [RDP_CMD_LOAD_TLUT] = 2,
        [RDP_CMD_SET_TILE_SIZE] = 2,
        [RDP_CMD_LOAD_BLOCK] = 2,
        [RDP_CMD_LOAD_TILE] = 2,
        [RDP_CMD_SET_TILE] = 2,
        [RDP_CMD_FILL_RECTANGLE] = 2,
        [RDP_CMD_SET_FILL_COLOR] = 2,
        [RDP_CMD_SET_FOG_COLOR] = 2,
        [RDP_CMD_SET_BLEND_COLOR] = 2,
        [RDP_CMD_SET_PRIM_COLOR] = 2,
        [RDP_CMD_SET_ENV_COLOR] = 2,
        [RDP_CMD_SET_COMBINE] = 2,
        [RDP_CMD_SET_TEXTURE_IMAGE] = 2,
        [RDP_CMD_SET_MASK_IMAGE] = 2,
        [RDP_CMD_SET_COLOR_IMAGE] = 2
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
    case RDP_CMD_FILL_RECTANGLE:
        return true;
    default:
        return false;
    }
}

const char *rdp_command_name(rdp_command_id id)
{
    static const char *names[64] = {
        [RDP_CMD_NO_OP] = "NO_OP",
        [RDP_CMD_FILL_TRIANGLE] = "FILL_TRIANGLE",
        [RDP_CMD_FILL_ZBUFFER_TRIANGLE] = "FILL_ZBUFFER_TRIANGLE",
        [RDP_CMD_TEXTURE_TRIANGLE] = "TEXTURE_TRIANGLE",
        [RDP_CMD_TEXTURE_ZBUFFER_TRIANGLE] = "TEXTURE_ZBUFFER_TRIANGLE",
        [RDP_CMD_SHADE_TRIANGLE] = "SHADE_TRIANGLE",
        [RDP_CMD_SHADE_ZBUFFER_TRIANGLE] = "SHADE_ZBUFFER_TRIANGLE",
        [RDP_CMD_SHADE_TEXTURE_TRIANGLE] = "SHADE_TEXTURE_TRIANGLE",
        [RDP_CMD_SHADE_TEXTURE_ZBUFFER_TRIANGLE] = "SHADE_TEXTURE_ZBUFFER_TRIANGLE",
        [RDP_CMD_TEXTURE_RECTANGLE] = "TEXTURE_RECTANGLE",
        [RDP_CMD_TEXTURE_RECTANGLE_FLIP] = "TEXTURE_RECTANGLE_FLIP",
        [RDP_CMD_SYNC_LOAD] = "SYNC_LOAD",
        [RDP_CMD_SYNC_PIPE] = "SYNC_PIPE",
        [RDP_CMD_SYNC_TILE] = "SYNC_TILE",
        [RDP_CMD_SYNC_FULL] = "SYNC_FULL",
        [RDP_CMD_SET_KEY_GB] = "SET_KEY_GB",
        [RDP_CMD_SET_KEY_R] = "SET_KEY_R",
        [RDP_CMD_SET_CONVERT] = "SET_CONVERT",
        [RDP_CMD_SET_SCISSOR] = "SET_SCISSOR",
        [RDP_CMD_SET_PRIM_DEPTH] = "SET_PRIM_DEPTH",
        [RDP_CMD_SET_OTHER_MODES] = "SET_OTHER_MODES",
        [RDP_CMD_LOAD_TLUT] = "LOAD_TLUT",
        [RDP_CMD_SET_TILE_SIZE] = "SET_TILE_SIZE",
        [RDP_CMD_LOAD_BLOCK] = "LOAD_BLOCK",
        [RDP_CMD_LOAD_TILE] = "LOAD_TILE",
        [RDP_CMD_SET_TILE] = "SET_TILE",
        [RDP_CMD_FILL_RECTANGLE] = "FILL_RECTANGLE",
        [RDP_CMD_SET_FILL_COLOR] = "SET_FILL_COLOR",
        [RDP_CMD_SET_FOG_COLOR] = "SET_FOG_COLOR",
        [RDP_CMD_SET_BLEND_COLOR] = "SET_BLEND_COLOR",
        [RDP_CMD_SET_PRIM_COLOR] = "SET_PRIM_COLOR",
        [RDP_CMD_SET_ENV_COLOR] = "SET_ENV_COLOR",
        [RDP_CMD_SET_COMBINE] = "SET_COMBINE",
        [RDP_CMD_SET_TEXTURE_IMAGE] = "SET_TEXTURE_IMAGE",
        [RDP_CMD_SET_MASK_IMAGE] = "SET_MASK_IMAGE",
        [RDP_CMD_SET_COLOR_IMAGE] = "SET_COLOR_IMAGE"
    };

    return id < 64 && names[id] ? names[id] : "UNKNOWN";
}

static void set_image(rdp_image *image, uint32_t w0, uint32_t w1)
{
    image->format = (rdp_texture_format)((w0 >> 21) & 7u);
    image->size = (rdp_texture_size)((w0 >> 19) & 3u);
    image->width = (w0 & 0x3ffu) + 1u;
    image->address = w1 & 0x00ffffffu;
}

static void set_other_modes(rdp_other_modes *modes, uint32_t w0, uint32_t w1)
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
    modes->image_read = (w1 & (1u << 6)) != 0;
    modes->z_update = (w1 & (1u << 5)) != 0;
    modes->z_compare = (w1 & (1u << 4)) != 0;
    modes->antialias = (w1 & (1u << 3)) != 0;
    modes->alpha_compare = (w1 & 1u) != 0;
    modes->coverage_dest = (uint8_t)((w1 >> 8) & 3u);
    modes->z_mode = (uint8_t)((w1 >> 10) & 3u);
}

static void set_tile(rdp_state *state, uint32_t w0, uint32_t w1)
{
    uint32_t index = (w1 >> 24) & 7u;
    rdp_tile *tile = &state->tiles[index];

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

static void set_tile_size(rdp_state *state, uint32_t w0, uint32_t w1)
{
    uint32_t index = (w1 >> 24) & 7u;
    rdp_tile *tile = &state->tiles[index];

    tile->sl = (uint16_t)((w0 >> 12) & 0xfffu);
    tile->tl = (uint16_t)(w0 & 0xfffu);
    tile->sh = (uint16_t)((w1 >> 12) & 0xfffu);
    tile->th = (uint16_t)(w1 & 0xfffu);
}

static void set_combine(rdp_state *state, uint32_t w0, uint32_t w1)
{
    const uint32_t rgb_muladd0 = (w0 >> 20) & 0xfu;
    const uint32_t rgb_mul0 = (w0 >> 15) & 0x1fu;
    const uint32_t rgb_mulsub0 = (w1 >> 28) & 0xfu;
    const uint32_t rgb_add0 = (w1 >> 15) & 0x7u;
    const uint32_t alpha_muladd0 = (w0 >> 12) & 0x7u;
    const uint32_t alpha_mulsub0 = (w1 >> 12) & 0x7u;
    const uint32_t alpha_mul0 = (w0 >> 9) & 0x7u;
    const uint32_t alpha_add0 = (w1 >> 9) & 0x7u;

    state->simple_combiner = RDP_SIMPLE_COMBINER_TEXEL0;

    if (rgb_muladd0 == 3u && rgb_mulsub0 == 8u && rgb_mul0 == 16u && rgb_add0 == 7u &&
        alpha_muladd0 == 3u && alpha_mulsub0 == 7u && alpha_mul0 == 7u && alpha_add0 == 7u) {
        state->simple_combiner = RDP_SIMPLE_COMBINER_PRIMITIVE;
    } else if (rgb_muladd0 == 1u && rgb_mulsub0 == 8u && rgb_mul0 == 4u && rgb_add0 == 7u &&
               alpha_muladd0 == 1u && alpha_mulsub0 == 7u && alpha_mul0 == 4u && alpha_add0 == 7u) {
        state->simple_combiner = RDP_SIMPLE_COMBINER_TEXEL0_SHADE;
    }
}

sr_result rdp_execute_command(sr_memory *memory, tmem_state *tmem, rdp_state *state, const rdp_command *cmd)
{
    const uint32_t w0 = cmd->words[0];
    const uint32_t w1 = cmd->words[1];

    state->commands_seen++;
    if (rdp_command_is_draw(cmd->id)) {
        state->draw_calls_seen++;
    }

    switch (cmd->id) {
    case RDP_CMD_NO_OP:
    case RDP_CMD_SYNC_LOAD:
    case RDP_CMD_SYNC_PIPE:
    case RDP_CMD_SYNC_TILE:
    case RDP_CMD_SYNC_FULL:
        return SR_OK;

    case RDP_CMD_SET_COLOR_IMAGE:
        set_image(&state->color_image, w0, w1);
        return SR_OK;

    case RDP_CMD_SET_TEXTURE_IMAGE:
        set_image(&state->texture_image, w0, w1);
        return SR_OK;

    case RDP_CMD_SET_MASK_IMAGE:
        state->depth_image_address = w1 & 0x00ffffffu;
        return SR_OK;

    case RDP_CMD_SET_SCISSOR:
        state->scissor_x0 = (uint16_t)((w0 >> 12) & 0xfffu);
        state->scissor_y0 = (uint16_t)(w0 & 0xfffu);
        state->scissor_x1 = (uint16_t)((w1 >> 12) & 0xfffu);
        state->scissor_y1 = (uint16_t)(w1 & 0xfffu);
        return SR_OK;

    case RDP_CMD_SET_OTHER_MODES:
        set_other_modes(&state->other_modes, w0, w1);
        return SR_OK;

    case RDP_CMD_SET_TILE:
        set_tile(state, w0, w1);
        return SR_OK;

    case RDP_CMD_SET_TILE_SIZE:
        set_tile_size(state, w0, w1);
        return SR_OK;

    case RDP_CMD_SET_FILL_COLOR:
        state->fill_color = w1;
        return SR_OK;

    case RDP_CMD_SET_FOG_COLOR:
        state->fog_color = color_from_word(w1);
        return SR_OK;

    case RDP_CMD_SET_BLEND_COLOR:
        state->blend_color = color_from_word(w1);
        return SR_OK;

    case RDP_CMD_SET_ENV_COLOR:
        state->environment_color = color_from_word(w1);
        return SR_OK;

    case RDP_CMD_SET_PRIM_COLOR:
        state->primitive_min_lod = (uint8_t)((w0 >> 8) & 0xffu);
        state->primitive_lod_fraction = (uint8_t)(w0 & 0xffu);
        state->primitive_color = color_from_word(w1);
        return SR_OK;

    case RDP_CMD_SET_PRIM_DEPTH:
        state->primitive_depth = (uint16_t)(w1 >> 16);
        state->primitive_delta_z = (uint16_t)w1;
        return SR_OK;

    case RDP_CMD_FILL_TRIANGLE:
    case RDP_CMD_FILL_ZBUFFER_TRIANGLE:
    case RDP_CMD_TEXTURE_TRIANGLE:
    case RDP_CMD_TEXTURE_ZBUFFER_TRIANGLE:
    case RDP_CMD_SHADE_TRIANGLE:
    case RDP_CMD_SHADE_ZBUFFER_TRIANGLE:
    case RDP_CMD_SHADE_TEXTURE_TRIANGLE:
    case RDP_CMD_SHADE_TEXTURE_ZBUFFER_TRIANGLE:
        return raster_submit_triangle(memory, tmem, state, cmd);

    case RDP_CMD_TEXTURE_RECTANGLE:
    case RDP_CMD_TEXTURE_RECTANGLE_FLIP:
    case RDP_CMD_FILL_RECTANGLE:
        return raster_submit_rectangle(memory, tmem, state, cmd);

    case RDP_CMD_LOAD_TLUT:
    case RDP_CMD_LOAD_BLOCK:
    case RDP_CMD_LOAD_TILE:
        return tmem_load_tile(tmem, memory, state, cmd);

    case RDP_CMD_SET_COMBINE:
        set_combine(state, w0, w1);
        return SR_OK;

    case RDP_CMD_SET_CONVERT:
    case RDP_CMD_SET_KEY_GB:
    case RDP_CMD_SET_KEY_R:
        return SR_OK;

    default:
        return SR_ERROR_BAD_COMMAND;
    }
}
