#ifndef SR_H
#define SR_H

#include "sr_defs.h"
#include "sr_host.h"
#include <stddef.h>

typedef struct sr_context sr_context;

typedef struct sr_vi_frame_info {
    uint32_t width;
    uint32_t height;
    uint32_t display_width;
    uint32_t display_height;
    bool display;
} sr_vi_frame_info;

typedef struct sr_debug_stats {
    uint32_t last_list_current;
    uint32_t last_list_end;
    uint32_t last_list_bytes;
    uint32_t last_command_address;
    uint32_t last_command_id;
    sr_result last_result;
    uint32_t color_image_format;
    uint32_t color_image_size;
    uint32_t color_image_width;
    uint32_t color_image_address;
    uint32_t last_texture_image_format;
    uint32_t last_texture_image_size;
    uint32_t last_texture_image_width;
    uint32_t last_texture_image_address;
    uint32_t last_tile_index;
    uint32_t last_tile_format;
    uint32_t last_tile_size;
    uint32_t last_tile_tmem;
    uint32_t last_tile_line;
    uint32_t last_tile_sl;
    uint32_t last_tile_tl;
    uint32_t last_tile_sh;
    uint32_t last_tile_th;
    uint32_t last_load_sl;
    uint32_t last_load_tl;
    uint32_t last_load_sh;
    uint32_t last_load_th;
    int32_t last_rect_s0;
    int32_t last_rect_t0;
    int32_t last_rect_dsdx;
    int32_t last_rect_dtdy;
} sr_debug_stats;

sr_context *sr_create(const sr_host_interface *host);
void sr_destroy(sr_context *ctx);

void sr_set_host(sr_context *ctx, const sr_host_interface *host);
sr_result sr_process_rdp_list(sr_context *ctx);
sr_result sr_get_vi_frame_info(sr_context *ctx, sr_vi_frame_info *info);
sr_result sr_update_screen(sr_context *ctx, sr_framebuffer *out);
sr_debug_stats sr_get_debug_stats(const sr_context *ctx);

/* Opaque, versioned RDP/TMEM state used by diagnostic frame dumps. */
size_t sr_state_snapshot_size(void);
sr_result sr_save_state(const sr_context *ctx, void *data, size_t size);
sr_result sr_load_state(sr_context *ctx, const void *data, size_t size);

#endif
