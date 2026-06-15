#ifndef RDP_COMMANDS_H
#define RDP_COMMANDS_H

#include "rdp_state.h"

typedef struct sr_memory sr_memory;
typedef struct tmem_state tmem_state;

typedef enum rdp_command_id {
    RDP_CMD_NO_OP = 0x00,
    RDP_CMD_FILL_TRIANGLE = 0x08,
    RDP_CMD_FILL_ZBUFFER_TRIANGLE = 0x09,
    RDP_CMD_TEXTURE_TRIANGLE = 0x0a,
    RDP_CMD_TEXTURE_ZBUFFER_TRIANGLE = 0x0b,
    RDP_CMD_SHADE_TRIANGLE = 0x0c,
    RDP_CMD_SHADE_ZBUFFER_TRIANGLE = 0x0d,
    RDP_CMD_SHADE_TEXTURE_TRIANGLE = 0x0e,
    RDP_CMD_SHADE_TEXTURE_ZBUFFER_TRIANGLE = 0x0f,
    RDP_CMD_TEXTURE_RECTANGLE = 0x24,
    RDP_CMD_TEXTURE_RECTANGLE_FLIP = 0x25,
    RDP_CMD_SYNC_LOAD = 0x26,
    RDP_CMD_SYNC_PIPE = 0x27,
    RDP_CMD_SYNC_TILE = 0x28,
    RDP_CMD_SYNC_FULL = 0x29,
    RDP_CMD_SET_KEY_GB = 0x2a,
    RDP_CMD_SET_KEY_R = 0x2b,
    RDP_CMD_SET_CONVERT = 0x2c,
    RDP_CMD_SET_SCISSOR = 0x2d,
    RDP_CMD_SET_PRIM_DEPTH = 0x2e,
    RDP_CMD_SET_OTHER_MODES = 0x2f,
    RDP_CMD_LOAD_TLUT = 0x30,
    RDP_CMD_SET_TILE_SIZE = 0x32,
    RDP_CMD_LOAD_BLOCK = 0x33,
    RDP_CMD_LOAD_TILE = 0x34,
    RDP_CMD_SET_TILE = 0x35,
    RDP_CMD_FILL_RECTANGLE = 0x36,
    RDP_CMD_SET_FILL_COLOR = 0x37,
    RDP_CMD_SET_FOG_COLOR = 0x38,
    RDP_CMD_SET_BLEND_COLOR = 0x39,
    RDP_CMD_SET_PRIM_COLOR = 0x3a,
    RDP_CMD_SET_ENV_COLOR = 0x3b,
    RDP_CMD_SET_COMBINE = 0x3c,
    RDP_CMD_SET_TEXTURE_IMAGE = 0x3d,
    RDP_CMD_SET_MASK_IMAGE = 0x3e,
    RDP_CMD_SET_COLOR_IMAGE = 0x3f
} rdp_command_id;

typedef struct rdp_command {
    rdp_command_id id;
    uint8_t word_count;
    uint32_t words[SR_MAX_COMMAND_WORDS];
} rdp_command;

uint8_t rdp_command_word_count(rdp_command_id id);
bool rdp_command_is_draw(rdp_command_id id);
const char *rdp_command_name(rdp_command_id id);
sr_result rdp_execute_command(sr_memory *memory, tmem_state *tmem, rdp_state *state, const rdp_command *cmd);

#endif
