#ifndef TMEM_H
#define TMEM_H

#include "rdp_state.h"
#include <stdbool.h>
#include <stdint.h>

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
sr_result tmem_load_tile(tmem_state *tmem, sr_memory *memory, rdp_state *state, const rdp_command *cmd);

static inline uint32_t shift_tile_coord(uint32_t coord, uint8_t shift)
{
    if (shift < 11u) {
        return coord >> shift;
    }
    if (shift < 16u) {
        return coord << (16u - shift);
    }
    return coord;
}

static inline bool resolve_tile_axis(uint32_t coord,
                                     uint32_t lo,
                                     uint32_t hi,
                                     uint32_t extent,
                                     bool clamp,
                                     bool mirror,
                                     uint8_t mask_bits,
                                     uint32_t *local)
{
    int32_t relative;

    if (clamp) {
        if (coord < lo) {
            relative = 0;
        } else if (coord > hi) {
            relative = (int32_t)extent - 1;
        } else {
            relative = (int32_t)(coord - lo);
        }
    } else {
        relative = (int32_t)(coord - lo);
    }

    if (mask_bits) {
        const int32_t period = 1 << mask_bits;
        if (mirror) {
            const int32_t mirror_bit = period;
            const int32_t reflected = (relative & mirror_bit) - 1;
            if (reflected > 0) {
                relative ^= reflected;
            }
        }
        relative &= period - 1;
    } else if (!clamp && (relative < 0 || relative >= (int32_t)extent)) {
        return false;
    }

    if (relative < 0 || relative >= (int32_t)extent) {
        return false;
    }

    *local = (uint32_t)relative;
    return true;
}

static inline bool tmem_sample_rgba5551(const tmem_state *tmem, const rdp_state *state, uint32_t tile_index, uint32_t s, uint32_t t, const rdp_tile_bounds *bounds, uint16_t *texel)
{
    if (!tmem || !state || !texel || !bounds || tile_index >= 8 ||
        tmem->tile_width[tile_index] == 0 || tmem->tile_height[tile_index] == 0) {
        return false;
    }

    const rdp_tile *tile = &state->tiles[tile_index];
    s = shift_tile_coord(s, tile->shift_s);
    t = shift_tile_coord(t, tile->shift_t);

    uint32_t local_s;
    uint32_t local_t;

    if (!resolve_tile_axis(s,
                           bounds->sl,
                           bounds->sh,
                           tmem->tile_width[tile_index],
                           tile->clamp_s != 0,
                           tile->mirror_s != 0,
                           tile->mask_s,
                           &local_s) ||
        !resolve_tile_axis(t,
                           bounds->tl,
                           bounds->th,
                           tmem->tile_height[tile_index],
                           tile->clamp_t != 0,
                           tile->mirror_t != 0,
                           tile->mask_t,
                           &local_t)) {
        return false;
    }

    if (local_s >= tmem->tile_width[tile_index] || local_t >= tmem->tile_height[tile_index]) {
        return false;
    }

    const uint32_t addr = tile->tmem + local_t * tmem->tile_stride[tile_index] + local_s * 2u;
    if (addr + 1u >= SR_TMEM_SIZE) {
        return false;
    }

    *texel = ((uint16_t)tmem->bytes[addr] << 8) | (uint16_t)tmem->bytes[addr + 1u];
    return true;
}

#endif
