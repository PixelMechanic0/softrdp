#ifndef RDP_METRICS_H
#define RDP_METRICS_H

#include <stdint.h>

/*
 * Renderer instrumentation is deliberately separate from emulated RDP state.
 * Updating diagnostics must never make register state mutable in the hot path.
 */
typedef struct rdp_metrics {
    uint64_t commands_seen;
    uint64_t draw_calls_seen;

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
} rdp_metrics;

#endif
