#ifndef SR_H
#define SR_H

#include "sr_defs.h"
#include "sr_host.h"

typedef struct sr_context sr_context;

typedef struct sr_debug_stats {
    uint64_t commands_seen;
    uint64_t draw_calls_seen;
} sr_debug_stats;

sr_context *sr_create(const sr_host_interface *host);
void sr_destroy(sr_context *ctx);

void sr_set_host(sr_context *ctx, const sr_host_interface *host);
sr_result sr_process_rdp_list(sr_context *ctx);
sr_result sr_update_screen(sr_context *ctx, sr_framebuffer *out);
sr_debug_stats sr_get_debug_stats(const sr_context *ctx);

#endif
