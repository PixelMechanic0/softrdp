#ifndef TMEM_H
#define TMEM_H

#include "rdp_state.h"

typedef struct rdp_command rdp_command;
typedef struct sr_memory sr_memory;

typedef struct tmem_state {
    uint8_t bytes[SR_TMEM_SIZE];
    uint16_t tile_width[8];
    uint16_t tile_height[8];
    uint16_t tile_stride[8];
    uint16_t tile_sl[8];
    uint16_t tile_tl[8];
    uint16_t tile_sh[8];
    uint16_t tile_th[8];
    uint64_t loads_seen;
} tmem_state;

void tmem_init(tmem_state *tmem);
sr_result tmem_load_tile(tmem_state *tmem, sr_memory *memory, const rdp_state *state, const rdp_command *cmd);
bool tmem_sample_rgba5551(const tmem_state *tmem, const rdp_state *state, uint32_t tile_index, uint32_t s, uint32_t t, uint16_t *texel);

#endif
