#include "sr.h"

#include "rdp_commands.h"
#include "rdp_memory.h"
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
    state->other_modes.bilerp0 = true;
    state->other_modes.bilerp1 = true;
    state->simple_combiner = RDP_SIMPLE_COMBINER_TEXEL0;
    state->combiner_needs_texel0 = true;
    state->combiner_needs_shade = false;
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
            result = rdp_execute_command(&ctx->memory, &ctx->tmem, &ctx->rdp, &cmd);
        }

        if (result != SR_OK) {
            ctx->debug.last_result = result;
            finish_rdp_list(ctx, end, true);
            return result;
        }

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
    ctx->rdp.process_rdp_ticks += (end.QuadPart - start.QuadPart);
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
    ctx->rdp.vi_ticks += (end.QuadPart - start.QuadPart);
#endif

    return result;
}

sr_debug_stats sr_get_debug_stats(const sr_context *ctx)
{
    sr_debug_stats stats = {0};

    if (ctx) {
        stats = ctx->debug;
        stats.commands_seen = ctx->rdp.commands_seen;
        stats.draw_calls_seen = ctx->rdp.draw_calls_seen;
#if SOFTRDP_ENABLE_PERF_LOG
        stats.triangle_count = ctx->rdp.triangle_count;
        stats.triangle_ticks = ctx->rdp.triangle_ticks;
        stats.rect_count = ctx->rdp.rect_count;
        stats.rect_ticks = ctx->rdp.rect_ticks;
        stats.tex_load_count = ctx->rdp.tex_load_count;
        stats.tex_load_ticks = ctx->rdp.tex_load_ticks;
        stats.vi_ticks = ctx->rdp.vi_ticks;
        stats.process_rdp_ticks = ctx->rdp.process_rdp_ticks;
#endif
    }

    return stats;
}
