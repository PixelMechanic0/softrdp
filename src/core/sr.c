#include "sr.h"

#include "rdp_commands.h"
#include "combiner.h"
#include "rdp_memory.h"
#include "rdp_state.h"
#include "tmem.h"
#include "vi.h"

#include <stdlib.h>
#include <string.h>

struct sr_context {
    sr_host_interface host;
    sr_memory memory;
    rdp_state rdp;
    tmem_state tmem;
    vi_state vi;
    vi_scanout_plan vi_plan;
    bool vi_plan_prepared;
    sr_debug_stats debug;

    /* Stateful command reader */
    uint32_t cmd_words[SR_MAX_COMMAND_WORDS];
    uint32_t cmd_word_count;
    uint32_t cmd_words_loaded;
    uint32_t cmd_current_address;
    uint8_t cmd_id;
};

#define SR_STATE_SNAPSHOT_MAGIC 0x31535253u /* "SRS1" */
typedef struct sr_state_snapshot {
    uint32_t magic;
    uint32_t size;
    rdp_state rdp;
    tmem_state tmem;
} sr_state_snapshot;

#define DP_STATUS_XBUS_DMA 0x001u
#define DP_INTERRUPT 0x20u
#define MAX_RDP_LIST_BYTES (1024u * 1024u)

static uint32_t read_reg(uint32_t *const *regs, uint32_t index)
{
    return regs[index] ? *regs[index] : 0;
}

size_t sr_state_snapshot_size(void)
{
    return sizeof(sr_state_snapshot);
}

sr_result sr_save_state(const sr_context *ctx, void *data, size_t size)
{
    if (!ctx || !data || size != sizeof(sr_state_snapshot)) return SR_ERROR_INVALID_ARGUMENT;
    sr_state_snapshot snapshot;
    memset(&snapshot, 0, sizeof(snapshot));
    snapshot.magic = SR_STATE_SNAPSHOT_MAGIC;
    snapshot.size = (uint32_t)sizeof(snapshot);
    snapshot.rdp = ctx->rdp;
    snapshot.tmem = ctx->tmem;
    memcpy(data, &snapshot, sizeof(snapshot));
    return SR_OK;
}

sr_result sr_load_state(sr_context *ctx, const void *data, size_t size)
{
    if (!ctx || !data || size != sizeof(sr_state_snapshot)) return SR_ERROR_INVALID_ARGUMENT;
    sr_state_snapshot snapshot;
    memcpy(&snapshot, data, sizeof(snapshot));
    if (snapshot.magic != SR_STATE_SNAPSHOT_MAGIC || snapshot.size != sizeof(snapshot))
        return SR_ERROR_INVALID_ARGUMENT;
    ctx->rdp = snapshot.rdp;
    ctx->tmem = snapshot.tmem;
    return SR_OK;
}

static void write_reg(uint32_t *const *regs, uint32_t index, uint32_t value)
{
    if (regs[index]) {
        *regs[index] = value;
    }
}

static void capture_texture_debug(sr_debug_stats *debug, const rdp_state *state,
                                  const rdp_command *cmd);

static sr_result execute_rdp_command(sr_context *ctx, rdp_command *cmd)
{
    sr_result result = rdp_decode_command(cmd);
    if (result == SR_OK)
        result = rdp_execute_command(&ctx->memory, &ctx->tmem, &ctx->rdp, cmd);
    if (result != SR_OK) {
        capture_texture_debug(&ctx->debug, &ctx->rdp, cmd);
        return result;
    }
    ctx->debug.color_image_format = (uint32_t)ctx->rdp.color_image.format;
    ctx->debug.color_image_size = (uint32_t)ctx->rdp.color_image.size;
    ctx->debug.color_image_width = ctx->rdp.color_image.width;
    ctx->debug.color_image_address = ctx->rdp.color_image.address;
    capture_texture_debug(&ctx->debug, &ctx->rdp, cmd);
    return SR_OK;
}

static bool read_command_word(const sr_context *ctx, bool xbus_dma, uint32_t word_index, uint32_t *word)
{
    if (xbus_dma) {
        const uint32_t byte_addr = (word_index & 0x3ffu) << 2;
        const uint8_t *dmem = ctx->host.dmem;
        if (!dmem || byte_addr + 3u >= SR_DMEM_SIZE) {
            return false;
        }

        if (ctx->memory.rdram_bswapped) {
            *word = ((uint32_t)dmem[byte_addr ^ 3u] << 24) |
                    ((uint32_t)dmem[(byte_addr + 1u) ^ 3u] << 16) |
                    ((uint32_t)dmem[(byte_addr + 2u) ^ 3u] << 8) |
                    ((uint32_t)dmem[(byte_addr + 3u) ^ 3u]);
        } else {
            *word = ((uint32_t)dmem[byte_addr] << 24) |
                    ((uint32_t)dmem[byte_addr + 1u] << 16) |
                    ((uint32_t)dmem[byte_addr + 2u] << 8) |
                    ((uint32_t)dmem[byte_addr + 3u]);
        }
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
    if (!ctx) return;
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
    bool full_sync_seen = false;

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
        finish_rdp_list(ctx, end, false);
        return SR_ERROR_BAD_COMMAND;
    }

    if (ctx->cmd_words_loaded == 0u) {
        uint32_t first_word;
        if (read_command_word(ctx, xbus_dma, current >> 2, &first_word)) {
            const uint8_t command_id = (uint8_t)((first_word >> 24) & 0x3fu);
            const uint32_t word_count = rdp_command_word_count((rdp_command_id)command_id);
            if (word_count != 0u && word_count <= SR_MAX_COMMAND_WORDS &&
                end - current == word_count * 4u) {
                rdp_command cmd;
                cmd.id = (rdp_command_id)command_id;
                cmd.word_count = (uint8_t)word_count;
                cmd.words[0] = first_word;
                for (uint32_t word = 1; word < word_count; word++) {
                    if (!read_command_word(ctx, xbus_dma, (current >> 2) + word,
                                           &cmd.words[word])) {
                        ctx->debug.last_result = SR_ERROR_BAD_COMMAND;
                        finish_rdp_list(ctx, end, false);
                        return SR_ERROR_BAD_COMMAND;
                    }
                }
                ctx->debug.last_command_address = current;
                ctx->debug.last_command_id = command_id;
                const sr_result result = execute_rdp_command(ctx, &cmd);
                ctx->debug.last_result = result;
                finish_rdp_list(ctx, end, result == SR_OK && cmd.id == RDP_CMD_SYNC_FULL);
                return result;
            }
        }
    }

    while (current < end) {
        if (ctx->cmd_words_loaded == 0) {
            uint32_t first_word = 0;
            if (!read_command_word(ctx, xbus_dma, current >> 2, &first_word)) {
                ctx->debug.last_command_address = current;
                ctx->debug.last_result = SR_ERROR_BAD_COMMAND;
                finish_rdp_list(ctx, end, false);
                return SR_ERROR_BAD_COMMAND;
            }

            ctx->cmd_id = (uint8_t)((first_word >> 24) & 0x3fu);
            ctx->cmd_word_count = rdp_command_word_count((rdp_command_id)ctx->cmd_id);
            if (ctx->cmd_word_count == 0 || ctx->cmd_word_count > SR_MAX_COMMAND_WORDS) {
                ctx->debug.last_result = SR_ERROR_BAD_COMMAND;
                finish_rdp_list(ctx, end, false);
                return SR_ERROR_BAD_COMMAND;
            }

            ctx->cmd_words[0] = first_word;
            ctx->cmd_words_loaded = 1;
            ctx->cmd_current_address = current;

            ctx->debug.last_command_address = ctx->cmd_current_address;
            ctx->debug.last_command_id = (uint32_t)ctx->cmd_id;

            current += 4u;
        }

        while (ctx->cmd_words_loaded < ctx->cmd_word_count) {
            if (current >= end) {
                /* Command is truncated in this list. Stop and wait for the next list. */
                finish_rdp_list(ctx, current, false);
                return SR_OK;
            }

            uint32_t word = 0;
            if (!read_command_word(ctx, xbus_dma, current >> 2, &word)) {
                ctx->debug.last_result = SR_ERROR_BAD_COMMAND;
                finish_rdp_list(ctx, end, false);
                return SR_ERROR_BAD_COMMAND;
            }

            ctx->cmd_words[ctx->cmd_words_loaded++] = word;
            current += 4u;
        }

        /* We have a complete command! Execute it. */
        rdp_command cmd;
        cmd.id = (rdp_command_id)ctx->cmd_id;
        cmd.word_count = (uint8_t)ctx->cmd_word_count;
        memcpy(cmd.words, ctx->cmd_words, ctx->cmd_word_count * sizeof(uint32_t));

        /* Reset stateful reader for next command */
        ctx->cmd_words_loaded = 0;

        sr_result result = execute_rdp_command(ctx, &cmd);

        if (result != SR_OK) {
            ctx->debug.last_result = result;
            finish_rdp_list(ctx, end, false);
            return result;
        }

        if (cmd.id == RDP_CMD_SYNC_FULL) {
            full_sync_seen = true;
        }

    }

    finish_rdp_list(ctx, end, full_sync_seen);
    return SR_OK;
}

sr_result sr_process_rdp_list(sr_context *ctx)
{
    if (!ctx) {
        return SR_ERROR_INVALID_ARGUMENT;
    }

    sr_result result = sr_process_rdp_list_internal(ctx);
    if (result == SR_OK) {
        vi_latch_registers(&ctx->vi, &ctx->host);
    }

    return result;
}

sr_result sr_update_screen(sr_context *ctx, sr_framebuffer *out)
{
    if (!ctx || !out) {
        return SR_ERROR_INVALID_ARGUMENT;
    }

    if (!ctx->vi_plan_prepared) {
        vi_latch_registers(&ctx->vi, &ctx->host);
        vi_build_scanout_plan(&ctx->vi, &ctx->memory, &ctx->vi_plan);
    }
    ctx->vi_plan_prepared = false;

    sr_result result = vi_execute_scanout(&ctx->vi_plan, &ctx->memory, out);

    return result;
}

sr_result sr_get_vi_frame_info(sr_context *ctx, sr_vi_frame_info *info)
{
    if (!ctx || !info) return SR_ERROR_INVALID_ARGUMENT;
    vi_latch_registers(&ctx->vi, &ctx->host);
    vi_build_scanout_plan(&ctx->vi, &ctx->memory, &ctx->vi_plan);
    ctx->vi_plan_prepared = true;
    info->width = ctx->vi_plan.output_width;
    info->height = ctx->vi_plan.output_height;
    info->display_width = ctx->vi_plan.display_width;
    info->display_height = ctx->vi_plan.display_height;
    info->display = ctx->vi_plan.state == VI_SCANOUT_READY;
    return SR_OK;
}

sr_debug_stats sr_get_debug_stats(const sr_context *ctx)
{
    sr_debug_stats stats = {0};

    if (ctx) {
        stats = ctx->debug;
    }

    return stats;
}
