#include "pj64_dump.h"

#if SOFTRDP_ENABLE_DUMP

#include "pj64_log.h"

#include "../../core/sr.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { FRAME_DUMP_PRESENT_COUNT = 4u };
enum { FRAME_DUMP_PAGE_SIZE = 4096u };
enum { FRAME_DUMP_MAX_PAGES = 2048u };

volatile bool pj64_dump_armed = false;

static const GFX_INFO *g_gfx;
static sr_context *g_ctx;
static uint32_t g_rdram_size;

static SRWLOCK g_lock = SRWLOCK_INIT;
static bool g_requested;
static bool g_record_active;
static FILE *g_file;
static uint8_t *g_rdram_shadow;
static uint32_t g_presents_remaining;
static uint32_t g_recorded_lists_count;
static long g_list_count_offset;

static uint32_t read_reg_value(DWORD *reg)
{
    return reg ? (uint32_t)*reg : 0u;
}

static void refresh_armed_locked(void)
{
    pj64_dump_armed = g_requested || g_record_active;
}

static void abort_locked(void)
{
    if (g_file) {
        fclose(g_file);
        g_file = NULL;
    }
    free(g_rdram_shadow);
    g_rdram_shadow = NULL;
    g_record_active = false;
    g_requested = false;
    refresh_armed_locked();
}

static bool write_all(FILE *file, const void *data, size_t size)
{
    const uint8_t *cursor = (const uint8_t *)data;
    while (size) {
        const size_t chunk = size > 64u * 1024u ? 64u * 1024u : size;
        const size_t written = fwrite(cursor, 1, chunk, file);
        if (written != chunk) return false;
        cursor += written;
        size -= written;
    }
    return true;
}

static bool write_record(uint32_t command_size,
                         const uint32_t dp_regs[8],
                         const uint32_t vi_regs[14],
                         uint32_t command_address,
                         bool xbus_dma)
{
    uint32_t changed[FRAME_DUMP_MAX_PAGES];
    uint32_t changed_count = 0;
    if (!g_file || !g_rdram_shadow || !g_gfx->RDRAM ||
        g_rdram_size / FRAME_DUMP_PAGE_SIZE > FRAME_DUMP_MAX_PAGES) return false;
    const uint32_t page_count = g_rdram_size / FRAME_DUMP_PAGE_SIZE;
    for (uint32_t page = 0; page < page_count; page++) {
        const uint32_t offset = page * FRAME_DUMP_PAGE_SIZE;
        if (memcmp(g_rdram_shadow + offset, g_gfx->RDRAM + offset,
                   FRAME_DUMP_PAGE_SIZE) != 0)
            changed[changed_count++] = page;
    }

    const size_t record_size = sizeof(changed_count) +
        (size_t)changed_count * (sizeof(uint32_t) + FRAME_DUMP_PAGE_SIZE) +
        sizeof(command_size) + 8u * sizeof(uint32_t) + 14u * sizeof(uint32_t) +
        command_size;
    uint8_t *record = malloc(record_size);
    if (!record) return false;

    uint8_t *cursor = record;
    memcpy(cursor, &changed_count, sizeof(changed_count));
    cursor += sizeof(changed_count);
    for (uint32_t i = 0; i < changed_count; i++) {
        const uint32_t page = changed[i];
        const uint32_t offset = page * FRAME_DUMP_PAGE_SIZE;
        memcpy(cursor, &page, sizeof(page));
        cursor += sizeof(page);
        memcpy(cursor, g_gfx->RDRAM + offset, FRAME_DUMP_PAGE_SIZE);
        memcpy(g_rdram_shadow + offset, cursor, FRAME_DUMP_PAGE_SIZE);
        cursor += FRAME_DUMP_PAGE_SIZE;
    }
    memcpy(cursor, &command_size, sizeof(command_size));
    cursor += sizeof(command_size);
    memcpy(cursor, dp_regs, 8u * sizeof(uint32_t));
    cursor += 8u * sizeof(uint32_t);
    memcpy(cursor, vi_regs, 14u * sizeof(uint32_t));
    cursor += 14u * sizeof(uint32_t);
    if (command_size) {
        if (xbus_dma) {
            for (uint32_t byte = 0; byte < command_size; byte++) {
                cursor[byte] = g_gfx->DMEM[(command_address + byte) & (SR_DMEM_SIZE - 1u)];
            }
        } else {
            memcpy(cursor, g_gfx->RDRAM + command_address, command_size);
        }
        cursor += command_size;
    }

    const bool ok = cursor == record + record_size &&
                    fwrite(record, 1, record_size, g_file) == record_size;
    free(record);
    return ok;
}

static void start_recording_locked(void)
{
    g_requested = false;
    g_record_active = true;
    g_recorded_lists_count = 0;
    g_presents_remaining = FRAME_DUMP_PRESENT_COUNT;
    refresh_armed_locked();

    g_file = fopen("frame_dump.bin", "wb");
    if (!g_file) {
        g_record_active = false;
        refresh_armed_locked();
        pj64_log_printf("ERROR: Could not open frame_dump.bin for writing!");
        return;
    }

    const uint32_t magic = 0x34444653; /* "SFD4" */
    const uint32_t rdram_size = g_rdram_size;
    const uint32_t bswapped = g_gfx->MemoryBswaped ? 1u : 0u;
    const uint32_t zero = 0;
    const uint32_t state_size = (uint32_t)sr_state_snapshot_size();
    void *state = malloc(state_size);

    fwrite(&magic, 4, 1, g_file);
    fwrite(&rdram_size, 4, 1, g_file);
    fwrite(&bswapped, 4, 1, g_file);
    g_list_count_offset = ftell(g_file);
    fwrite(&zero, 4, 1, g_file); /* list_count placeholder */
    fwrite(&state_size, 4, 1, g_file);
    if (state && sr_save_state(g_ctx, state, state_size) == SR_OK) {
        fwrite(state, 1, state_size, g_file);
    } else {
        pj64_log_printf("ERROR: Could not capture RDP state for frame dump");
        free(state);
        abort_locked();
        return;
    }
    free(state);

    if (!write_all(g_file, g_gfx->RDRAM, g_rdram_size)) {
        pj64_log_printf("ERROR: Could not write initial RDRAM to frame dump (errno=%d)", errno);
        abort_locked();
        return;
    }

    free(g_rdram_shadow);
    g_rdram_shadow = malloc(g_rdram_size);
    if (!g_rdram_shadow) {
        abort_locked();
        return;
    }
    memcpy(g_rdram_shadow, g_gfx->RDRAM, g_rdram_size);

    pj64_log_printf("Starting frame dump recording for %u presented frames...",
                      FRAME_DUMP_PRESENT_COUNT);
}

static void finish_recording_locked(void)
{
    g_record_active = false;
    refresh_armed_locked();
    if (!g_file) {
        return;
    }

    /* Final reference RDRAM output. */
    const bool wrote_rdram = write_all(g_file, g_gfx->RDRAM, g_rdram_size);
    const bool sought_header = wrote_rdram &&
                               fseek(g_file, g_list_count_offset, SEEK_SET) == 0;
    const bool wrote_count = sought_header &&
                             fwrite(&g_recorded_lists_count, 4, 1, g_file) == 1;
    const bool closed = fclose(g_file) == 0;
    g_file = NULL;
    free(g_rdram_shadow);
    g_rdram_shadow = NULL;

    if (wrote_rdram && wrote_count && closed)
        pj64_log_printf("Frame dump completed successfully! Recorded %u lists.",
                          g_recorded_lists_count);
    else
        pj64_log_printf("ERROR: Frame dump incomplete (rdram=%d header=%d close=%d errno=%d)",
                          wrote_rdram ? 1 : 0, wrote_count ? 1 : 0,
                          closed ? 1 : 0, errno);
}

void pj64_dump_attach(const GFX_INFO *gfx, sr_context *ctx, uint32_t rdram_size)
{
    AcquireSRWLockExclusive(&g_lock);
    abort_locked();
    g_gfx = gfx;
    g_ctx = ctx;
    g_rdram_size = rdram_size;
    ReleaseSRWLockExclusive(&g_lock);
}

void pj64_dump_detach(void)
{
    AcquireSRWLockExclusive(&g_lock);
    abort_locked();
    g_gfx = NULL;
    g_ctx = NULL;
    g_rdram_size = 0;
    ReleaseSRWLockExclusive(&g_lock);
}

void pj64_dump_poll_hotkey(void)
{
    static bool f11_was_down = false;
    const bool f11_is_down = (GetAsyncKeyState(VK_F11) & 0x8000) != 0;
    const bool pressed = f11_is_down && !f11_was_down;
    f11_was_down = f11_is_down;

    if (!pressed) {
        return;
    }

    AcquireSRWLockExclusive(&g_lock);
    if (g_gfx && g_ctx && !g_record_active) {
        g_requested = true;
        refresh_armed_locked();
        pj64_log_printf("F11 pressed: Requesting frame dump on next RDP list");
    }
    ReleaseSRWLockExclusive(&g_lock);
}

void pj64_dump_before_list(void)
{
    AcquireSRWLockExclusive(&g_lock);
    if (!g_gfx) {
        ReleaseSRWLockExclusive(&g_lock);
        return;
    }

    if (g_requested) {
        start_recording_locked();
    }

    if (g_record_active && g_file) {
        const uint32_t current = read_reg_value(g_gfx->DPC_CURRENT_REG) & ~7u;
        const uint32_t end = read_reg_value(g_gfx->DPC_END_REG) & ~7u;
        const bool xbus_dma = (read_reg_value(g_gfx->DPC_STATUS_REG) & 0x001u) != 0u;
        uint32_t size = 0;
        if (end > current) {
            if (xbus_dma && g_gfx->DMEM && end - current <= SR_DMEM_SIZE) {
                size = end - current;
            } else if (!xbus_dma && current < g_rdram_size && end <= g_rdram_size) {
                size = end - current;
            }
        }

        const uint32_t dp_regs[8] = {
            read_reg_value(g_gfx->DPC_START_REG),
            read_reg_value(g_gfx->DPC_END_REG),
            read_reg_value(g_gfx->DPC_CURRENT_REG),
            read_reg_value(g_gfx->DPC_STATUS_REG),
            read_reg_value(g_gfx->DPC_CLOCK_REG),
            read_reg_value(g_gfx->DPC_BUFBUSY_REG),
            read_reg_value(g_gfx->DPC_PIPEBUSY_REG),
            read_reg_value(g_gfx->DPC_TMEM_REG),
        };

        const uint32_t vi_regs[14] = {
            read_reg_value(g_gfx->VI_STATUS_REG),
            read_reg_value(g_gfx->VI_ORIGIN_REG),
            read_reg_value(g_gfx->VI_WIDTH_REG),
            read_reg_value(g_gfx->VI_INTR_REG),
            read_reg_value(g_gfx->VI_V_CURRENT_LINE_REG),
            read_reg_value(g_gfx->VI_TIMING_REG),
            read_reg_value(g_gfx->VI_V_SYNC_REG),
            read_reg_value(g_gfx->VI_H_SYNC_REG),
            read_reg_value(g_gfx->VI_LEAP_REG),
            read_reg_value(g_gfx->VI_H_START_REG),
            read_reg_value(g_gfx->VI_V_START_REG),
            read_reg_value(g_gfx->VI_V_BURST_REG),
            read_reg_value(g_gfx->VI_X_SCALE_REG),
            read_reg_value(g_gfx->VI_Y_SCALE_REG),
        };

        if (write_record(size, dp_regs, vi_regs, current, xbus_dma)) {
            g_recorded_lists_count++;
        } else {
            pj64_log_printf("ERROR: Could not write atomic frame dump record");
            abort_locked();
        }
    }
    ReleaseSRWLockExclusive(&g_lock);
}

void pj64_dump_after_list(void)
{
    AcquireSRWLockExclusive(&g_lock);
    if (g_record_active && g_rdram_shadow && g_gfx && g_gfx->RDRAM) {
        /* RDP writes must be reproduced independently by each replay renderer,
         * not injected into the next record as host deltas. */
        memcpy(g_rdram_shadow, g_gfx->RDRAM, g_rdram_size);
    }
    ReleaseSRWLockExclusive(&g_lock);
}

void pj64_dump_on_present(void)
{
    AcquireSRWLockExclusive(&g_lock);
    if (g_record_active) {
        if (g_presents_remaining > 0u) {
            g_presents_remaining--;
        }
        if (g_presents_remaining == 0u) {
            finish_recording_locked();
        }
    }
    ReleaseSRWLockExclusive(&g_lock);
}

#endif
