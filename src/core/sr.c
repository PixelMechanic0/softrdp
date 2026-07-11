#include "sr.h"

#include "rdp_commands.h"
#include "combiner.h"
#include "rdp_memory.h"
#include "rdp_metrics.h"
#include "rdp_state.h"
#include "tmem.h"
#include "vi.h"

#include <stdlib.h>
#include <string.h>

#if SOFTRDP_ENABLE_PERF_LOG
#define NOMINMAX
#include <windows.h>
#endif

struct sr_context {
    sr_host_interface host;
    sr_memory memory;
    rdp_state rdp;
    tmem_state tmem;
    vi_state vi;
    rdp_metrics metrics;
    sr_debug_stats debug;
};

#define DP_STATUS_XBUS_DMA 0x001u
#define DP_INTERRUPT 0x20u
#define MAX_RDP_LIST_BYTES (1024u * 1024u)

static uint32_t read_reg(uint32_t *const *regs, uint32_t index)
{
    return regs[index] ? *regs[index] : 0;
}

static void write_reg(uint32_t *const *regs, uint32_t index, uint32_t value)
{
    if (regs[index]) {
        *regs[index] = value;
    }
}

static bool read_command_word(const sr_context *ctx, bool xbus_dma, uint32_t word_index, uint32_t *word)
{
    if (xbus_dma) {
        const uint32_t byte_addr = (word_index & 0x3ffu) << 2;
        const uint8_t *dmem = ctx->host.dmem;
        if (!dmem || byte_addr + 3u >= SR_DMEM_SIZE) {
            return false;
        }

        *word = ((uint32_t)dmem[byte_addr] << 24) |
                ((uint32_t)dmem[byte_addr + 1u] << 16) |
                ((uint32_t)dmem[byte_addr + 2u] << 8) |
                ((uint32_t)dmem[byte_addr + 3u]);
        return true;
    }

    return sr_memory_read_be32(&ctx->memory, word_index << 2, word);
}

static void finish_rdp_list(sr_context *ctx, uint32_t end, bool interrupt)
{
    write_reg(ctx->host.dp_regs, SR_DP_START, end);
    write_reg(ctx->host.dp_regs, SR_DP_CURRENT, end);
    write_reg(ctx->host.dp_regs, SR_DP_END, end);

    if (interrupt) {
        if (ctx->host.mi_intr_reg) {
            *ctx->host.mi_intr_reg |= DP_INTERRUPT;
        }
        if (ctx->host.raise_mi_interrupt) {
            ctx->host.raise_mi_interrupt(ctx->host.userdata);
        }
    }
}

static void rdp_state_init(rdp_state *state)
{
    memset(state, 0, sizeof(*state));

    state->color_image.format = RDP_FORMAT_RGBA;
    state->color_image.size = RDP_SIZE_16BPP;
    state->color_image.width = 320;

    state->texture_image.format = RDP_FORMAT_RGBA;
    state->texture_image.size = RDP_SIZE_16BPP;
    state->texture_image.width = 1;

    state->scissor_x1 = 640u << 2;
    state->scissor_y1 = 480u << 2;
    state->other_modes.cycle_type = RDP_CYCLE_1;
    rdp_combiner_make_passthrough(&state->combiner,
                                  RDP_COMBINER_TEXEL0_RGB,
                                  RDP_COMBINER_TEXEL0_ALPHA);
}

static void capture_texture_debug(sr_debug_stats *debug, const rdp_state *state, const rdp_command *cmd)
{
    if (!debug || !state || !cmd) {
        return;
    }

    uint32_t tile_index = 0;
    debug->last_texture_image_format = (uint32_t)state->texture_image.format;
    debug->last_texture_image_size = (uint32_t)state->texture_image.size;
    debug->last_texture_image_width = state->texture_image.width;
    debug->last_texture_image_address = state->texture_image.address;
    debug->last_load_sl = 0;
    debug->last_load_tl = 0;
    debug->last_load_sh = 0;
    debug->last_load_th = 0;
    debug->last_rect_s0 = 0;
    debug->last_rect_t0 = 0;
    debug->last_rect_dsdx = 0;
    debug->last_rect_dtdy = 0;

    switch (cmd->id) {
    case RDP_CMD_LOAD_TLUT:
    case RDP_CMD_LOAD_BLOCK:
    case RDP_CMD_LOAD_TILE:
        tile_index = cmd->decoded.load.tile_index;
        debug->last_load_sl = cmd->decoded.load.sl;
        debug->last_load_tl = cmd->decoded.load.tl;
        debug->last_load_sh = cmd->decoded.load.sh;
        debug->last_load_th = cmd->decoded.load.th;
        break;
    case RDP_CMD_TEXTURE_RECTANGLE:
    case RDP_CMD_TEXTURE_RECTANGLE_FLIP:
        tile_index = cmd->decoded.rect.tile_index;
        debug->last_rect_s0 = cmd->decoded.rect.s0;
        debug->last_rect_t0 = cmd->decoded.rect.t0;
        debug->last_rect_dsdx = cmd->decoded.rect.dsdx;
        debug->last_rect_dtdy = cmd->decoded.rect.dtdy;
        break;
    case RDP_CMD_TEXTURE_TRIANGLE:
    case RDP_CMD_TEXTURE_ZBUFFER_TRIANGLE:
    case RDP_CMD_SHADE_TEXTURE_TRIANGLE:
    case RDP_CMD_SHADE_TEXTURE_ZBUFFER_TRIANGLE:
        tile_index = cmd->decoded.triangle.position.tile & 7u;
        break;
    default:
        return;
    }

    tile_index &= 7u;
    const rdp_tile *tile = &state->tiles[tile_index];
    debug->last_tile_index = tile_index;
    debug->last_tile_format = (uint32_t)tile->format;
    debug->last_tile_size = (uint32_t)tile->size;
    debug->last_tile_tmem = tile->tmem;
    debug->last_tile_line = tile->line;
    debug->last_tile_sl = tile->sl;
    debug->last_tile_tl = tile->tl;
    debug->last_tile_sh = tile->sh;
    debug->last_tile_th = tile->th;
}

sr_context *sr_create(const sr_host_interface *host)
{
    sr_context *ctx = calloc(1, sizeof(*ctx));
    if (!ctx) {
        return NULL;
    }

    sr_set_host(ctx, host);
    rdp_state_init(&ctx->rdp);
    tmem_init(&ctx->tmem);
    vi_init(&ctx->vi);
    return ctx;
}

void sr_destroy(sr_context *ctx)
{
    free(ctx);
}

void sr_set_host(sr_context *ctx, const sr_host_interface *host)
{
    if (!ctx) {
        return;
    }

    if (host) {
        ctx->host = *host;
    } else {
        memset(&ctx->host, 0, sizeof(ctx->host));
    }

    sr_memory_init(&ctx->memory, &ctx->host);
}

static sr_result sr_process_rdp_list_internal(sr_context *ctx)
{
    uint32_t current;
    uint32_t end;
    bool xbus_dma;

    current = read_reg(ctx->host.dp_regs, SR_DP_CURRENT) & ~7u;
    end = read_reg(ctx->host.dp_regs, SR_DP_END) & ~7u;
    xbus_dma = (read_reg(ctx->host.dp_regs, SR_DP_STATUS) & DP_STATUS_XBUS_DMA) != 0;
    ctx->debug.last_list_current = current;
    ctx->debug.last_list_end = end;
    ctx->debug.last_list_bytes = end > current ? end - current : 0u;
    ctx->debug.last_command_address = current;
    ctx->debug.last_command_id = 0u;
    ctx->debug.last_result = SR_OK;

    if (end <= current) {
        return SR_OK;
    }
    if (end - current > MAX_RDP_LIST_BYTES) {
        ctx->debug.last_result = SR_ERROR_BAD_COMMAND;
        finish_rdp_list(ctx, end, true);
        return SR_ERROR_BAD_COMMAND;
    }

    while (current < end) {
        rdp_command cmd;
        uint32_t first_word = 0;
        uint32_t current_word = current >> 2;

        if (!read_command_word(ctx, xbus_dma, current_word, &first_word)) {
            ctx->debug.last_command_address = current;
            ctx->debug.last_result = SR_ERROR_BAD_COMMAND;
            finish_rdp_list(ctx, end, true);
            return SR_ERROR_BAD_COMMAND;
        }

        cmd.id = (rdp_command_id)((first_word >> 24) & 0x3fu);
        ctx->debug.last_command_address = current;
        ctx->debug.last_command_id = (uint32_t)cmd.id;
        cmd.word_count = rdp_command_word_count(cmd.id);
        if (cmd.word_count == 0 || cmd.word_count > SR_MAX_COMMAND_WORDS) {
            ctx->debug.last_result = SR_ERROR_BAD_COMMAND;
            finish_rdp_list(ctx, end, true);
            return SR_ERROR_BAD_COMMAND;
        }

        for (uint8_t i = 0; i < cmd.word_count; i++) {
            if (!read_command_word(ctx, xbus_dma, current_word + i, &cmd.words[i])) {
                ctx->debug.last_result = SR_ERROR_BAD_COMMAND;
                finish_rdp_list(ctx, end, true);
                return SR_ERROR_BAD_COMMAND;
            }
        }

        sr_result result = rdp_decode_command(&cmd);
        if (result == SR_OK) {
            result = rdp_execute_command(&ctx->memory, &ctx->tmem, &ctx->rdp, &ctx->metrics, &cmd);
        }

        if (result != SR_OK) {
            ctx->debug.last_result = result;
            capture_texture_debug(&ctx->debug, &ctx->rdp, &cmd);
            finish_rdp_list(ctx, end, true);
            return result;
        }
        capture_texture_debug(&ctx->debug, &ctx->rdp, &cmd);

        current += (uint32_t)cmd.word_count * 4u;
    }

    finish_rdp_list(ctx, end, true);
    return SR_OK;
}

sr_result sr_process_rdp_list(sr_context *ctx)
{
    if (!ctx) {
        return SR_ERROR_INVALID_ARGUMENT;
    }

#if SOFTRDP_ENABLE_PERF_LOG
    LARGE_INTEGER start, end;
    QueryPerformanceCounter(&start);
#endif

    sr_result result = sr_process_rdp_list_internal(ctx);

#if SOFTRDP_ENABLE_PERF_LOG
    QueryPerformanceCounter(&end);
    ctx->metrics.process_rdp_ticks += (end.QuadPart - start.QuadPart);
#endif

    return result;
}

sr_result sr_update_screen(sr_context *ctx, sr_framebuffer *out)
{
    if (!ctx) {
        return SR_ERROR_INVALID_ARGUMENT;
    }

    vi_latch_registers(&ctx->vi, &ctx->host);

#if SOFTRDP_ENABLE_PERF_LOG
    LARGE_INTEGER start, end;
    QueryPerformanceCounter(&start);
#endif

    sr_result result = vi_scanout(&ctx->vi, &ctx->memory, out);

#if SOFTRDP_ENABLE_PERF_LOG
    QueryPerformanceCounter(&end);
    ctx->metrics.vi_ticks += (end.QuadPart - start.QuadPart);
#endif

    return result;
}

sr_debug_stats sr_get_debug_stats(const sr_context *ctx)
{
    sr_debug_stats stats = {0};

    if (ctx) {
        stats = ctx->debug;
        stats.commands_seen = ctx->metrics.commands_seen;
        stats.draw_calls_seen = ctx->metrics.draw_calls_seen;
        memcpy(stats.command_counts, ctx->metrics.command_counts, sizeof(stats.command_counts));
        stats.depth_tests = ctx->metrics.depth_tests;
        stats.depth_passes = ctx->metrics.depth_passes;
        stats.depth_rejects = ctx->metrics.depth_rejects;
        stats.depth_updates_planned = ctx->metrics.depth_updates_planned;
        stats.depth_updates_committed = ctx->metrics.depth_updates_committed;
        stats.depth_updates_discarded = ctx->metrics.depth_updates_discarded;
#if SOFTRDP_ENABLE_PERF_LOG
        stats.triangle_count = ctx->metrics.triangle_count;
        stats.triangle_ticks = ctx->metrics.triangle_ticks;
        stats.rect_count = ctx->metrics.rect_count;
        stats.rect_ticks = ctx->metrics.rect_ticks;
        stats.tex_load_count = ctx->metrics.tex_load_count;
        stats.tex_load_ticks = ctx->metrics.tex_load_ticks;
        stats.texture_sample_attempts = ctx->metrics.texture_sample_attempts;
        stats.texture_sample_hits = ctx->metrics.texture_sample_hits;
        stats.texture_sample_misses = ctx->metrics.texture_sample_misses;
        stats.texture_sample_shade_fallbacks = ctx->metrics.texture_sample_shade_fallbacks;
        memcpy(stats.texture_sample_by_format_size,
               ctx->metrics.texture_sample_by_format_size,
               sizeof(stats.texture_sample_by_format_size));
        memcpy(stats.texture_sample_hits_by_format_size,
               ctx->metrics.texture_sample_hits_by_format_size,
               sizeof(stats.texture_sample_hits_by_format_size));
        stats.texture_sample_tlut_attempts = ctx->metrics.texture_sample_tlut_attempts;
        stats.texture_sample_bilerp_attempts = ctx->metrics.texture_sample_bilerp_attempts;
        stats.texture_sample_quad_attempts = ctx->metrics.texture_sample_quad_attempts;
        stats.texture_sample_mid_texel_attempts = ctx->metrics.texture_sample_mid_texel_attempts;
        stats.texture_sample_perspective_attempts = ctx->metrics.texture_sample_perspective_attempts;
        stats.texture_sample_texel0_shade_attempts = ctx->metrics.texture_sample_texel0_shade_attempts;
        stats.texture_sample_min_s = ctx->metrics.texture_sample_min_s;
        stats.texture_sample_max_s = ctx->metrics.texture_sample_max_s;
        stats.texture_sample_min_t = ctx->metrics.texture_sample_min_t;
        stats.texture_sample_max_t = ctx->metrics.texture_sample_max_t;
        stats.texture_sample_color_xor = ctx->metrics.texture_sample_color_xor;
        stats.texture_sample_min_s_fixed = ctx->metrics.texture_sample_min_s_fixed;
        stats.texture_sample_max_s_fixed = ctx->metrics.texture_sample_max_s_fixed;
        stats.texture_sample_min_t_fixed = ctx->metrics.texture_sample_min_t_fixed;
        stats.texture_sample_max_t_fixed = ctx->metrics.texture_sample_max_t_fixed;
        stats.texture_sample_min_w_fixed = ctx->metrics.texture_sample_min_w_fixed;
        stats.texture_sample_max_w_fixed = ctx->metrics.texture_sample_max_w_fixed;
        stats.rect_texture_sample_attempts = ctx->metrics.rect_texture_sample_attempts;
        stats.rect_texture_sample_hits = ctx->metrics.rect_texture_sample_hits;
        stats.rect_texture_sample_misses = ctx->metrics.rect_texture_sample_misses;
        stats.tex_load_block_count = ctx->metrics.tex_load_block_count;
        stats.tex_load_tile_count = ctx->metrics.tex_load_tile_count;
        stats.tex_load_tlut_count = ctx->metrics.tex_load_tlut_count;
        memcpy(stats.tex_load_by_format_size,
               ctx->metrics.tex_load_by_format_size,
               sizeof(stats.tex_load_by_format_size));
        stats.vi_ticks = ctx->metrics.vi_ticks;
        stats.process_rdp_ticks = ctx->metrics.process_rdp_ticks;
#endif
    }

    return stats;
}
