#ifndef SR_H
#define SR_H

#include "sr_defs.h"
#include "sr_host.h"

typedef struct sr_context sr_context;

typedef struct sr_debug_stats {
    uint64_t commands_seen;
    uint64_t draw_calls_seen;
    uint32_t last_list_current;
    uint32_t last_list_end;
    uint32_t last_list_bytes;
    uint32_t last_command_address;
    uint32_t last_command_id;
    sr_result last_result;

    uint64_t triangle_count;
    uint64_t triangle_ticks;
    uint64_t rect_count;
    uint64_t rect_ticks;
    uint64_t tex_load_count;
    uint64_t tex_load_ticks;
    uint64_t vi_ticks;
    uint64_t process_rdp_ticks;
} sr_debug_stats;

sr_context *sr_create(const sr_host_interface *host);
void sr_destroy(sr_context *ctx);

void sr_set_host(sr_context *ctx, const sr_host_interface *host);
sr_result sr_process_rdp_list(sr_context *ctx);
sr_result sr_update_screen(sr_context *ctx, sr_framebuffer *out);
sr_debug_stats sr_get_debug_stats(const sr_context *ctx);

#endif
