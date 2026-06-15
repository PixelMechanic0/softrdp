#ifndef RASTER_H
#define RASTER_H

#include "rdp_state.h"

typedef struct rdp_command rdp_command;
typedef struct sr_memory sr_memory;
typedef struct tmem_state tmem_state;

typedef struct raster_triangle_setup {
    int32_t xh;
    int32_t xm;
    int32_t xl;
    int16_t yh;
    int16_t ym;
    int16_t yl;
    int32_t dxhdy;
    int32_t dxmdy;
    int32_t dxldy;
    uint8_t tile;
    bool flip;
} raster_triangle_setup;

typedef struct raster_shade_setup {
    int32_t r, g, b, a;
    int32_t drdx, dgdx, dbdx, dadx;
    int32_t drde, dgde, dbde, dade;
    int32_t drdy, dgdy, dbdy, dady;
} raster_shade_setup;

typedef struct raster_texture_setup {
    int32_t s, t, w;
    int32_t dsdx, dtdx, dwdx;
    int32_t dsde, dtde, dwde;
    int32_t dsdy, dtdy, dwdy;
} raster_texture_setup;

typedef struct raster_depth_setup {
    int32_t z;
    int32_t dzdx;
    int32_t dzde;
    int32_t dzdy;
} raster_depth_setup;

typedef struct raster_decoded_triangle {
    raster_triangle_setup position;
    raster_shade_setup shade;
    raster_texture_setup texture;
    raster_depth_setup depth;
    bool has_shade;
    bool has_texture;
    bool has_depth;
} raster_decoded_triangle;

sr_result raster_decode_triangle(const rdp_command *cmd, raster_decoded_triangle *out);
sr_result raster_submit_triangle(sr_memory *memory, tmem_state *tmem, rdp_state *state, const rdp_command *cmd);
sr_result raster_submit_rectangle(sr_memory *memory, tmem_state *tmem, rdp_state *state, const rdp_command *cmd);

#endif
