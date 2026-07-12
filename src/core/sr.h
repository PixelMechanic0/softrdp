#ifndef SR_H
#define SR_H

#include "sr_defs.h"
#include "sr_host.h"

typedef struct sr_context sr_context;

typedef struct sr_vi_frame_info {
    uint32_t width;
    uint32_t height;
    uint32_t display_width;
    uint32_t display_height;
    bool display;
} sr_vi_frame_info;

typedef struct sr_debug_stats {
    uint64_t commands_seen;
    uint64_t draw_calls_seen;
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

    uint64_t triangle_count;
    uint64_t triangle_ticks;
    uint64_t rect_count;
    uint64_t rect_ticks;
    uint64_t tex_load_count;
    uint64_t tex_load_ticks;
    uint64_t texture_sample_attempts;
    uint64_t texture_sample_hits;
    uint64_t texture_sample_misses;
    uint64_t texture_sample_shade_fallbacks;
    uint64_t texture_sample_by_format_size[5][4];
    uint64_t texture_sample_hits_by_format_size[5][4];
    uint64_t texture_sample_tlut_attempts;
    uint64_t texture_sample_bilerp_attempts;
    uint64_t texture_sample_quad_attempts;
    uint64_t texture_sample_mid_texel_attempts;
    uint64_t texture_sample_perspective_attempts;
    uint64_t texture_sample_texel0_shade_attempts;
    uint32_t texture_sample_min_s;
    uint32_t texture_sample_max_s;
    uint32_t texture_sample_min_t;
    uint32_t texture_sample_max_t;
    uint32_t texture_sample_color_xor;
    int32_t texture_sample_min_s_fixed;
    int32_t texture_sample_max_s_fixed;
    int32_t texture_sample_min_t_fixed;
    int32_t texture_sample_max_t_fixed;
    int32_t texture_sample_min_w_fixed;
    int32_t texture_sample_max_w_fixed;
    uint64_t rect_texture_sample_attempts;
    uint64_t rect_texture_sample_hits;
    uint64_t rect_texture_sample_misses;
    uint64_t fragment_attempts;
    uint64_t fragment_alpha_rejects;
    uint64_t fragment_depth_tests;
    uint64_t fragment_depth_rejects;
    uint64_t fragment_writes;
    uint32_t fragment_color_xor;
    uint32_t fragment_min_address;
    uint32_t fragment_max_address;
    uint64_t tex_load_block_count;
    uint64_t tex_load_tile_count;
    uint64_t tex_load_tlut_count;
    uint64_t tex_load_by_format_size[5][4];
    uint64_t vi_ticks;
    uint64_t process_rdp_ticks;
} sr_debug_stats;

sr_context *sr_create(const sr_host_interface *host);
void sr_destroy(sr_context *ctx);

void sr_set_host(sr_context *ctx, const sr_host_interface *host);
sr_result sr_process_rdp_list(sr_context *ctx);
sr_result sr_get_vi_frame_info(sr_context *ctx, sr_vi_frame_info *info);
sr_result sr_update_screen(sr_context *ctx, sr_framebuffer *out);
sr_debug_stats sr_get_debug_stats(const sr_context *ctx);

#endif
