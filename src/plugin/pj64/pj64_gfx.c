#include "pj64_gfx.h"
#include "pj64_log.h"

#include "../../core/rdp_commands.h"
#include "../../core/sr.h"
#include "../../present/sr_present.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

#define PJ64_MAX_FRAME_WIDTH 800u
#define PJ64_MAX_FRAME_HEIGHT 600u

static GFX_INFO g_gfx;
static sr_context *g_context;
static sr_present g_present;
static sr_rgba8 *g_frame_pixels;
static uint32_t g_frame_capacity_pixels;
static uint32_t g_frame_width = 640u;
static uint32_t g_frame_height = 480u;
static uint32_t g_display_width = 640u;
static uint32_t g_display_height = 480u;
static bool g_runtime_started;
static uint32_t g_process_dlist_calls;
static uint32_t g_process_rdp_calls;
static uint32_t g_update_screen_calls;
static uint32_t g_uploaded_frames;

static bool g_dump_requested = false;
static bool g_record_active = false;
static FILE* g_frame_dump_file = NULL;
static uint32_t g_rdram_size = 0x400000;
enum { FRAME_DUMP_PRESENT_COUNT = 4u };
static uint32_t g_dump_presents_remaining = 0;
static uint8_t *g_dump_rdram_shadow = NULL;
enum { FRAME_DUMP_PAGE_SIZE = 4096u };

static bool dump_write_all(FILE *file, const void *data, size_t size)
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
static uint32_t g_recorded_lists_count = 0;
static long g_list_count_offset = 0;

#if SOFTRDP_ENABLE_PERF_LOG
static sr_debug_stats g_perf_baseline_stats;
static uint32_t g_perf_frame_count;
static double g_perf_draw_total_ms;
#endif

#if SOFTRDP_ENABLE_PERF_OVERLAY
typedef struct perf_overlay_state {
    sr_debug_stats last_stats;
    LARGE_INTEGER last_update;
    LARGE_INTEGER rate_window_start;
    LARGE_INTEGER last_text_update;
    uint32_t rate_updates;
    double vi_per_second;
    double triangle_ms;
    double rect_ms;
    double tmem_ms;
    double other_ms;
    double vi_ms;
    double present_ms;
    bool initialized;
} perf_overlay_state;
static perf_overlay_state g_overlay;

static double smooth_value(double previous, double sample)
{
    return previous == 0.0 ? sample : previous + (sample - previous) * 0.1;
}

static void prepare_perf_overlay(void)
{
    LARGE_INTEGER now, frequency;
    QueryPerformanceCounter(&now);
    QueryPerformanceFrequency(&frequency);
    const sr_debug_stats stats = sr_get_debug_stats(g_context);
    if (g_overlay.initialized) {
        const uint64_t rdp = stats.process_rdp_ticks - g_overlay.last_stats.process_rdp_ticks;
        const uint64_t tri_ticks = stats.triangle_ticks - g_overlay.last_stats.triangle_ticks;
        const uint64_t rect_ticks = stats.rect_ticks - g_overlay.last_stats.rect_ticks;
        const uint64_t tmem_ticks = stats.tex_load_ticks - g_overlay.last_stats.tex_load_ticks;
        const uint64_t tri_count = stats.triangle_count - g_overlay.last_stats.triangle_count;
        const uint64_t rect_count = stats.rect_count - g_overlay.last_stats.rect_count;
        const uint64_t tmem_count = stats.tex_load_count - g_overlay.last_stats.tex_load_count;
        const uint64_t tri_samples = stats.triangle_sample_count - g_overlay.last_stats.triangle_sample_count;
        const uint64_t rect_samples = stats.rect_sample_count - g_overlay.last_stats.rect_sample_count;
        const uint64_t tmem_samples = stats.tex_load_sample_count - g_overlay.last_stats.tex_load_sample_count;
        const uint64_t vi = stats.vi_ticks - g_overlay.last_stats.vi_ticks;
        const double to_ms = 1000.0 / (double)frequency.QuadPart;
        g_overlay.rate_updates++;
        const double rate_elapsed = (double)(now.QuadPart -
                                             g_overlay.rate_window_start.QuadPart);
        if (rate_elapsed >= (double)frequency.QuadPart) {
            g_overlay.vi_per_second = (double)g_overlay.rate_updates *
                                      (double)frequency.QuadPart / rate_elapsed;
            g_overlay.rate_window_start = now;
            g_overlay.rate_updates = 0;
        }
        const bool detail_complete =
            (tri_count == 0 || tri_samples != 0) &&
            (rect_count == 0 || rect_samples != 0) &&
            (tmem_count == 0 || tmem_samples != 0);
        if (detail_complete) {
            const double tri_est = tri_samples ?
                (double)tri_ticks * (double)tri_count / (double)tri_samples : 0.0;
            const double rect_est = rect_samples ?
                (double)rect_ticks * (double)rect_count / (double)rect_samples : 0.0;
            const double tmem_est = tmem_samples ?
                (double)tmem_ticks * (double)tmem_count / (double)tmem_samples : 0.0;
            const double accounted = tri_est + rect_est + tmem_est;
            const double other = (double)rdp > accounted ? (double)rdp - accounted : 0.0;
            g_overlay.triangle_ms = smooth_value(g_overlay.triangle_ms, tri_est * to_ms);
            g_overlay.rect_ms = smooth_value(g_overlay.rect_ms, rect_est * to_ms);
            g_overlay.tmem_ms = smooth_value(g_overlay.tmem_ms, tmem_est * to_ms);
            g_overlay.other_ms = smooth_value(g_overlay.other_ms, other * to_ms);
        }
        g_overlay.vi_ms = smooth_value(g_overlay.vi_ms, vi * to_ms);
    }
    g_overlay.last_stats = stats;
    g_overlay.last_update = now;
    if (!g_overlay.initialized) g_overlay.rate_window_start = now;
    g_overlay.initialized = true;

    if (g_overlay.last_text_update.QuadPart == 0 ||
        now.QuadPart - g_overlay.last_text_update.QuadPart >= frequency.QuadPart / 4) {
        char text[256];
        snprintf(text, sizeof(text),
                 "VI/S %5.1f\nTRI %6.2f\nRECT %5.2f\nTMEM %5.2f\nOTHER %4.2f\nVI %7.2f\nPRESENT %2.2f",
                 g_overlay.vi_per_second, g_overlay.triangle_ms,
                 g_overlay.rect_ms, g_overlay.tmem_ms, g_overlay.other_ms,
                 g_overlay.vi_ms, g_overlay.present_ms);
        sr_present_set_overlay_text(&g_present, text);
        g_overlay.last_text_update = now;
    }
}

static void record_present_time(LARGE_INTEGER start, LARGE_INTEGER end)
{
    LARGE_INTEGER frequency;
    QueryPerformanceFrequency(&frequency);
    const double sample = (double)(end.QuadPart - start.QuadPart) * 1000.0 /
                          (double)frequency.QuadPart;
    g_overlay.present_ms = smooth_value(g_overlay.present_ms, sample);
}
#endif

static uint32_t *reg32(DWORD *reg)
{
    return (uint32_t *)(void *)reg;
}

static void raise_mi_interrupt(void *userdata)
{
    GFX_INFO *gfx = userdata;
    if (gfx && gfx->MI_INTR_REG) {
        *gfx->MI_INTR_REG |= 0x20u;
    }
    if (gfx && gfx->CheckInterrupts) {
        gfx->CheckInterrupts();
    }
}

static void acknowledge_dp_list(void)
{
    if (g_gfx.DPC_END_REG) {
        if (g_gfx.DPC_START_REG) {
            *g_gfx.DPC_START_REG = *g_gfx.DPC_END_REG;
        }
        if (g_gfx.DPC_CURRENT_REG) {
            *g_gfx.DPC_CURRENT_REG = *g_gfx.DPC_END_REG;
        }
    }

    raise_mi_interrupt(&g_gfx);
}

static uint32_t read_reg_value(DWORD *reg)
{
    return reg ? (uint32_t)*reg : 0;
}

static bool ensure_frame_storage(uint32_t width, uint32_t height)
{
    const uint32_t pixels = width * height;
    sr_rgba8 *new_pixels;

    if (width == 0 || height == 0 || width > PJ64_MAX_FRAME_WIDTH ||
        height > PJ64_MAX_FRAME_HEIGHT) {
        return false;
    }

    if (pixels <= g_frame_capacity_pixels) {
        return true;
    }

    new_pixels = realloc(g_frame_pixels, pixels * sizeof(*g_frame_pixels));
    if (!new_pixels) {
        return false;
    }

    g_frame_pixels = new_pixels;
    g_frame_capacity_pixels = pixels;
    return true;
}

static void release_frame_storage(void)
{
    free(g_frame_pixels);
    g_frame_pixels = NULL;
    g_frame_capacity_pixels = 0;
}

#if SOFTRDP_ENABLE_LOG
static const char *result_name(sr_result result)
{
    switch (result) {
    case SR_OK:
        return "OK";
    case SR_ERROR_INVALID_ARGUMENT:
        return "INVALID_ARGUMENT";
    case SR_ERROR_BAD_COMMAND:
        return "BAD_COMMAND";
    case SR_ERROR_UNSUPPORTED:
        return "UNSUPPORTED";
    default:
        return "UNKNOWN";
    }
}
#endif

static bool dump_write_rdram_deltas(FILE *file)
{
    uint32_t changed[2048];
    uint32_t changed_count = 0;
    if (!file || !g_dump_rdram_shadow || !g_gfx.RDRAM ||
        g_rdram_size / FRAME_DUMP_PAGE_SIZE > 2048u) return false;
    const uint32_t page_count = g_rdram_size / FRAME_DUMP_PAGE_SIZE;
    for (uint32_t page = 0; page < page_count; page++) {
        const uint32_t offset = page * FRAME_DUMP_PAGE_SIZE;
        if (memcmp(g_dump_rdram_shadow + offset, g_gfx.RDRAM + offset,
                   FRAME_DUMP_PAGE_SIZE) != 0)
            changed[changed_count++] = page;
    }
    if (fwrite(&changed_count, 4, 1, file) != 1) return false;
    for (uint32_t i = 0; i < changed_count; i++) {
        const uint32_t page = changed[i];
        const uint32_t offset = page * FRAME_DUMP_PAGE_SIZE;
        if (fwrite(&page, 4, 1, file) != 1 ||
            !dump_write_all(file, g_gfx.RDRAM + offset, FRAME_DUMP_PAGE_SIZE)) return false;
        memcpy(g_dump_rdram_shadow + offset, g_gfx.RDRAM + offset, FRAME_DUMP_PAGE_SIZE);
    }
    return true;
}

static bool command_has_texture_debug(uint32_t command_id)
{
    switch ((rdp_command_id)command_id) {
    case RDP_CMD_LOAD_TLUT:
    case RDP_CMD_LOAD_BLOCK:
    case RDP_CMD_LOAD_TILE:
    case RDP_CMD_TEXTURE_RECTANGLE:
    case RDP_CMD_TEXTURE_RECTANGLE_FLIP:
    case RDP_CMD_TEXTURE_TRIANGLE:
    case RDP_CMD_TEXTURE_ZBUFFER_TRIANGLE:
    case RDP_CMD_SHADE_TEXTURE_TRIANGLE:
    case RDP_CMD_SHADE_TEXTURE_ZBUFFER_TRIANGLE:
        return true;
    default:
        return false;
    }
}

static void log_texture_debug(const sr_debug_stats *stats)
{
    if (!stats || !command_has_texture_debug(stats->last_command_id)) {
        return;
    }

    pj64_log_printf("  tex-debug ti fmt=%u size=%u width=%u addr=%08x tile=%u fmt=%u size=%u tmem=%u line=%u tile_st=%u,%u-%u,%u",
                      stats->last_texture_image_format,
                      stats->last_texture_image_size,
                      stats->last_texture_image_width,
                      stats->last_texture_image_address,
                      stats->last_tile_index,
                      stats->last_tile_format,
                      stats->last_tile_size,
                      stats->last_tile_tmem,
                      stats->last_tile_line,
                      stats->last_tile_sl,
                      stats->last_tile_tl,
                      stats->last_tile_sh,
                      stats->last_tile_th);

    if (stats->last_command_id == RDP_CMD_LOAD_TLUT ||
        stats->last_command_id == RDP_CMD_LOAD_BLOCK ||
        stats->last_command_id == RDP_CMD_LOAD_TILE) {
        pj64_log_printf("  load-debug sl=%u tl=%u sh=%u th/dxt=%u",
                          stats->last_load_sl,
                          stats->last_load_tl,
                          stats->last_load_sh,
                          stats->last_load_th);
    } else if (stats->last_command_id == RDP_CMD_TEXTURE_RECTANGLE ||
               stats->last_command_id == RDP_CMD_TEXTURE_RECTANGLE_FLIP) {
        pj64_log_printf("  rect-debug s0=%d t0=%d dsdx=%d dtdy=%d",
                          stats->last_rect_s0,
                          stats->last_rect_t0,
                          stats->last_rect_dsdx,
                          stats->last_rect_dtdy);
    }
}

static bool log_sample_u32(uint32_t count, uint32_t early_count, uint32_t interval)
{
    return count <= early_count || (interval != 0u && (count % interval) == 0u);
}

static sr_host_interface make_host_interface(GFX_INFO *gfx)
{
    sr_host_interface host;
    memset(&host, 0, sizeof(host));

    host.rdram = gfx->RDRAM;
#ifdef _WIN32
    {
        MEMORY_BASIC_INFORMATION mbi;
        if (VirtualQuery(gfx->RDRAM, &mbi, sizeof(mbi)) != 0) {
            char *base = (char *)mbi.BaseAddress;
            size_t offset = (char *)gfx->RDRAM - base;
            if (mbi.RegionSize > offset) {
                size_t size = mbi.RegionSize - offset;
                if (size >= 0x800000) {
                    g_rdram_size = 0x800000;
                } else if (size >= 0x400000) {
                    g_rdram_size = 0x400000;
                }
            }
        }
    }
#endif
    host.rdram_size = g_rdram_size;
    host.rdram_bswapped = gfx->MemoryBswaped != 0;
    host.dmem = gfx->DMEM;
    host.mi_intr_reg = reg32(gfx->MI_INTR_REG);
    host.raise_mi_interrupt = raise_mi_interrupt;
    host.userdata = gfx;

    host.dp_regs[SR_DP_START] = reg32(gfx->DPC_START_REG);
    host.dp_regs[SR_DP_END] = reg32(gfx->DPC_END_REG);
    host.dp_regs[SR_DP_CURRENT] = reg32(gfx->DPC_CURRENT_REG);
    host.dp_regs[SR_DP_STATUS] = reg32(gfx->DPC_STATUS_REG);
    host.dp_regs[SR_DP_CLOCK] = reg32(gfx->DPC_CLOCK_REG);
    host.dp_regs[SR_DP_BUFBUSY] = reg32(gfx->DPC_BUFBUSY_REG);
    host.dp_regs[SR_DP_PIPEBUSY] = reg32(gfx->DPC_PIPEBUSY_REG);
    host.dp_regs[SR_DP_TMEM] = reg32(gfx->DPC_TMEM_REG);

    host.vi_regs[SR_VI_STATUS] = reg32(gfx->VI_STATUS_REG);
    host.vi_regs[SR_VI_ORIGIN] = reg32(gfx->VI_ORIGIN_REG);
    host.vi_regs[SR_VI_WIDTH] = reg32(gfx->VI_WIDTH_REG);
    host.vi_regs[SR_VI_INTR] = reg32(gfx->VI_INTR_REG);
    host.vi_regs[SR_VI_CURRENT] = reg32(gfx->VI_V_CURRENT_LINE_REG);
    host.vi_regs[SR_VI_TIMING] = reg32(gfx->VI_TIMING_REG);
    host.vi_regs[SR_VI_V_SYNC] = reg32(gfx->VI_V_SYNC_REG);
    host.vi_regs[SR_VI_H_SYNC] = reg32(gfx->VI_H_SYNC_REG);
    host.vi_regs[SR_VI_LEAP] = reg32(gfx->VI_LEAP_REG);
    host.vi_regs[SR_VI_H_START] = reg32(gfx->VI_H_START_REG);
    host.vi_regs[SR_VI_V_START] = reg32(gfx->VI_V_START_REG);
    host.vi_regs[SR_VI_V_BURST] = reg32(gfx->VI_V_BURST_REG);
    host.vi_regs[SR_VI_X_SCALE] = reg32(gfx->VI_X_SCALE_REG);
    host.vi_regs[SR_VI_Y_SCALE] = reg32(gfx->VI_Y_SCALE_REG);

    return host;
}

static void ensure_window_size(HWND hwnd)
{
#ifdef _WIN32
    if (!hwnd) {
        return;
    }
    RECT rect;
    if (GetClientRect(hwnd, &rect)) {
        int width = rect.right - rect.left;
        int height = rect.bottom - rect.top;
        if (width != 800 || height != 600) {
            RECT wr = {0, 0, 800, 600};
            DWORD style = (DWORD)GetWindowLongA(hwnd, GWL_STYLE);
            DWORD ex_style = (DWORD)GetWindowLongA(hwnd, GWL_EXSTYLE);
            BOOL menu = (GetMenu(hwnd) != NULL);
            AdjustWindowRectEx(&wr, style, menu, ex_style);
            SetWindowPos(hwnd, NULL, 0, 0, wr.right - wr.left, wr.bottom - wr.top,
                         SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
        }
    }
#else
    (void)hwnd;
#endif
}

static bool start_runtime(void)
{
    sr_host_interface host;

    if (g_runtime_started) {
        return true;
    }

    ensure_window_size(g_gfx.hWnd);

    pj64_log_open();
    pj64_log_printf("RomOpen/start_runtime hwnd=%p swapped=%d rdram=%p dmem=%p",
                      (void *)g_gfx.hWnd,
                      g_gfx.MemoryBswaped ? 1 : 0,
                      (void *)g_gfx.RDRAM,
                      (void *)g_gfx.DMEM);

    host = make_host_interface(&g_gfx);
    sr_destroy(g_context);
    g_context = sr_create(&host);
    if (!g_context) {
        pj64_log_printf("start_runtime: sr_create failed");
        return false;
    }

    if (!sr_present_init(&g_present, g_gfx.hWnd)) {
        pj64_log_printf("start_runtime: OpenGL presentation init failed");
        MessageBoxA(g_gfx.hWnd,
                    "SoftRDP could not initialize the OpenGL 3.3 presentation backend.",
                    "SoftRDP",
                    MB_ICONERROR | MB_OK);
        return false;
    }

    g_runtime_started = true;
    pj64_log_printf("start_runtime: ready");
#if SOFTRDP_ENABLE_PERF_LOG
    g_perf_baseline_stats = sr_get_debug_stats(g_context);
    g_perf_frame_count = 0;
    g_perf_draw_total_ms = 0.0;
#endif
    return true;
}

#if SOFTRDP_ENABLE_PERF_LOG
static const char *texture_format_name(uint32_t format)
{
    switch (format) {
    case 0: return "RGBA";
    case 1: return "YUV";
    case 2: return "CI";
    case 3: return "IA";
    case 4: return "I";
    default: return "?";
    }
}

static const char *texture_size_name(uint32_t size)
{
    switch (size) {
    case 0: return "4b";
    case 1: return "8b";
    case 2: return "16b";
    case 3: return "32b";
    default: return "?";
    }
}

static void log_format_size_delta(const char *label,
                                  const uint64_t current[5][4],
                                  const uint64_t baseline[5][4])
{
    char line[512];
    size_t used = 0;
    bool any = false;

    used += (size_t)snprintf(line + used, sizeof(line) - used, "      %s:", label);
    for (uint32_t format = 0; format < 5; format++) {
        for (uint32_t size = 0; size < 4; size++) {
            const uint64_t delta = current[format][size] - baseline[format][size];
            if (delta == 0) {
                continue;
            }
            any = true;
            if (used + 64u >= sizeof(line)) {
                line[sizeof(line) - 1u] = '\0';
                pj64_log_printf("%s", line);
                used = 0;
                used += (size_t)snprintf(line + used, sizeof(line) - used, "      %s:", label);
            }
            used += (size_t)snprintf(line + used,
                                     sizeof(line) - used,
                                     " %s/%s=%llu",
                                     texture_format_name(format),
                                     texture_size_name(size),
                                     (unsigned long long)delta);
        }
    }

    if (any) {
        pj64_log_printf("%s", line);
    }
}

static void log_perf_summary(const char *header)
{
    if (!g_context || g_perf_frame_count == 0) {
        return;
    }

    sr_debug_stats current_stats = sr_get_debug_stats(g_context);
    LARGE_INTEGER freq;
    QueryPerformanceFrequency(&freq);
    double freq_d = (double)freq.QuadPart;

    uint64_t delta_rdp_ticks = current_stats.process_rdp_ticks - g_perf_baseline_stats.process_rdp_ticks;
    uint64_t delta_tri_ticks = current_stats.triangle_ticks - g_perf_baseline_stats.triangle_ticks;
    uint64_t delta_tri_count = current_stats.triangle_count - g_perf_baseline_stats.triangle_count;
    uint64_t delta_rect_ticks = current_stats.rect_ticks - g_perf_baseline_stats.rect_ticks;
    uint64_t delta_rect_count = current_stats.rect_count - g_perf_baseline_stats.rect_count;
    uint64_t delta_tex_ticks = current_stats.tex_load_ticks - g_perf_baseline_stats.tex_load_ticks;
    uint64_t delta_tex_count = current_stats.tex_load_count - g_perf_baseline_stats.tex_load_count;
    uint64_t delta_sample_attempts = current_stats.texture_sample_attempts - g_perf_baseline_stats.texture_sample_attempts;
    uint64_t delta_sample_hits = current_stats.texture_sample_hits - g_perf_baseline_stats.texture_sample_hits;
    uint64_t delta_sample_misses = current_stats.texture_sample_misses - g_perf_baseline_stats.texture_sample_misses;
    uint64_t delta_sample_fallbacks = current_stats.texture_sample_shade_fallbacks - g_perf_baseline_stats.texture_sample_shade_fallbacks;
    uint64_t delta_sample_tlut = current_stats.texture_sample_tlut_attempts - g_perf_baseline_stats.texture_sample_tlut_attempts;
    uint64_t delta_sample_bilerp = current_stats.texture_sample_bilerp_attempts - g_perf_baseline_stats.texture_sample_bilerp_attempts;
    uint64_t delta_sample_quad = current_stats.texture_sample_quad_attempts - g_perf_baseline_stats.texture_sample_quad_attempts;
    uint64_t delta_sample_mid = current_stats.texture_sample_mid_texel_attempts - g_perf_baseline_stats.texture_sample_mid_texel_attempts;
    uint64_t delta_sample_perspective = current_stats.texture_sample_perspective_attempts - g_perf_baseline_stats.texture_sample_perspective_attempts;
    uint64_t delta_sample_texelshade = current_stats.texture_sample_texel0_shade_attempts - g_perf_baseline_stats.texture_sample_texel0_shade_attempts;
    uint64_t delta_rect_sample_attempts = current_stats.rect_texture_sample_attempts - g_perf_baseline_stats.rect_texture_sample_attempts;
    uint64_t delta_rect_sample_hits = current_stats.rect_texture_sample_hits - g_perf_baseline_stats.rect_texture_sample_hits;
    uint64_t delta_rect_sample_misses = current_stats.rect_texture_sample_misses - g_perf_baseline_stats.rect_texture_sample_misses;
    uint64_t delta_fragment_attempts = current_stats.fragment_attempts - g_perf_baseline_stats.fragment_attempts;
    uint64_t delta_fragment_alpha_rejects = current_stats.fragment_alpha_rejects - g_perf_baseline_stats.fragment_alpha_rejects;
    uint64_t delta_fragment_depth_tests = current_stats.fragment_depth_tests - g_perf_baseline_stats.fragment_depth_tests;
    uint64_t delta_fragment_depth_rejects = current_stats.fragment_depth_rejects - g_perf_baseline_stats.fragment_depth_rejects;
    uint64_t delta_fragment_writes = current_stats.fragment_writes - g_perf_baseline_stats.fragment_writes;
    uint64_t delta_load_blocks = current_stats.tex_load_block_count - g_perf_baseline_stats.tex_load_block_count;
    uint64_t delta_load_tiles = current_stats.tex_load_tile_count - g_perf_baseline_stats.tex_load_tile_count;
    uint64_t delta_load_tluts = current_stats.tex_load_tlut_count - g_perf_baseline_stats.tex_load_tlut_count;
    uint64_t delta_vi_ticks = current_stats.vi_ticks - g_perf_baseline_stats.vi_ticks;
    uint64_t delta_commands = current_stats.commands_seen - g_perf_baseline_stats.commands_seen;
    uint64_t delta_draws = current_stats.draw_calls_seen - g_perf_baseline_stats.draw_calls_seen;

    double rdp_ms = (double)delta_rdp_ticks * 1000.0 / freq_d;
    double tri_ms = (double)delta_tri_ticks * 1000.0 / freq_d;
    double rect_ms = (double)delta_rect_ticks * 1000.0 / freq_d;
    double tex_ms = (double)delta_tex_ticks * 1000.0 / freq_d;
    double vi_ms = (double)delta_vi_ticks * 1000.0 / freq_d;

    pj64_log_printf("PERF SUMMARY - %s (over %u frames):", header, g_perf_frame_count);
    pj64_log_printf("  RDP processing:       %8.3f ms (avg %7.3f ms/frame)", rdp_ms, rdp_ms / g_perf_frame_count);
    pj64_log_printf("    Included timers; these overlap with RDP processing total:");
    pj64_log_printf("    - Triangles:  %5llu calls, %8.3f ms (avg %7.3f ms/tri)",
                      (unsigned long long)delta_tri_count, tri_ms, delta_tri_count ? tri_ms / delta_tri_count : 0.0);
    pj64_log_printf("    - Rectangles: %5llu calls, %8.3f ms (avg %7.3f ms/rect)",
                      (unsigned long long)delta_rect_count, rect_ms, delta_rect_count ? rect_ms / delta_rect_count : 0.0);
    pj64_log_printf("    - Tex Loads:  %5llu calls, %8.3f ms (avg %7.3f ms/load)",
                      (unsigned long long)delta_tex_count, tex_ms, delta_tex_count ? tex_ms / delta_tex_count : 0.0);
    pj64_log_printf("      load cmds: block=%llu tile=%llu tlut=%llu",
                      (unsigned long long)delta_load_blocks,
                      (unsigned long long)delta_load_tiles,
                      (unsigned long long)delta_load_tluts);
    log_format_size_delta("load formats",
                          current_stats.tex_load_by_format_size,
                          g_perf_baseline_stats.tex_load_by_format_size);
    pj64_log_printf("    - Tex Samples: tri attempts=%llu hits=%llu misses=%llu shade_fallbacks=%llu rect attempts=%llu hits=%llu misses=%llu",
                      (unsigned long long)delta_sample_attempts,
                      (unsigned long long)delta_sample_hits,
                      (unsigned long long)delta_sample_misses,
                      (unsigned long long)delta_sample_fallbacks,
                      (unsigned long long)delta_rect_sample_attempts,
                      (unsigned long long)delta_rect_sample_hits,
                      (unsigned long long)delta_rect_sample_misses);
    pj64_log_printf("      sample modes: tlut=%llu bilerp0=%llu sample_quad=%llu mid_texel=%llu perspective=%llu texel0*shade=%llu",
                      (unsigned long long)delta_sample_tlut,
                      (unsigned long long)delta_sample_bilerp,
                      (unsigned long long)delta_sample_quad,
                      (unsigned long long)delta_sample_mid,
                      (unsigned long long)delta_sample_perspective,
                      (unsigned long long)delta_sample_texelshade);
    log_format_size_delta("sample attempts",
                          current_stats.texture_sample_by_format_size,
                          g_perf_baseline_stats.texture_sample_by_format_size);
    log_format_size_delta("sample hits",
                          current_stats.texture_sample_hits_by_format_size,
                          g_perf_baseline_stats.texture_sample_hits_by_format_size);
    pj64_log_printf("      sample-range s=%u..%u t=%u..%u color_xor=%08x",
                      current_stats.texture_sample_min_s,
                      current_stats.texture_sample_max_s,
                      current_stats.texture_sample_min_t,
                      current_stats.texture_sample_max_t,
                      current_stats.texture_sample_color_xor);
    pj64_log_printf("      sample-fixed s=%d..%d t=%d..%d w=%d..%d",
                      current_stats.texture_sample_min_s_fixed,
                      current_stats.texture_sample_max_s_fixed,
                      current_stats.texture_sample_min_t_fixed,
                      current_stats.texture_sample_max_t_fixed,
                      current_stats.texture_sample_min_w_fixed,
                      current_stats.texture_sample_max_w_fixed);
    pj64_log_printf("    - Fragments: attempts=%llu alpha_rejects=%llu depth=%llu/%llu writes=%llu color_xor=%08x address=%08x..%08x",
                      (unsigned long long)delta_fragment_attempts,
                      (unsigned long long)delta_fragment_alpha_rejects,
                      (unsigned long long)delta_fragment_depth_rejects,
                      (unsigned long long)delta_fragment_depth_tests,
                      (unsigned long long)delta_fragment_writes,
                      current_stats.fragment_color_xor,
                      current_stats.fragment_min_address,
                      current_stats.fragment_max_address);
    pj64_log_printf("  Total VI Scanout:     %8.3f ms (avg %7.3f ms/frame)", vi_ms, vi_ms / g_perf_frame_count);
    pj64_log_printf("  Total Present/Draw:   %8.3f ms (avg %7.3f ms/frame)", g_perf_draw_total_ms, g_perf_draw_total_ms / g_perf_frame_count);
    pj64_log_printf("  Total Commands: %llu, Draw Calls: %llu",
                      (unsigned long long)delta_commands, (unsigned long long)delta_draws);

    g_perf_baseline_stats = current_stats;
    g_perf_frame_count = 0;
    g_perf_draw_total_ms = 0.0;
}
#endif

static void stop_runtime(void)
{
#if SOFTRDP_ENABLE_PERF_LOG
    log_perf_summary("Shutdown");
#endif
    if (g_runtime_started || pj64_log_is_open()) {
        pj64_log_printf("stop_runtime rdp_calls=%u update_calls=%u uploaded_frames=%u",
                          g_process_rdp_calls,
                          g_update_screen_calls,
                          g_uploaded_frames);
    }
    sr_present_shutdown(&g_present);
    release_frame_storage();
    sr_destroy(g_context);
    g_context = NULL;
    g_runtime_started = false;
    g_process_rdp_calls = 0;
    g_process_dlist_calls = 0;
    g_update_screen_calls = 0;
    g_uploaded_frames = 0;
#if SOFTRDP_ENABLE_PERF_OVERLAY
    memset(&g_overlay, 0, sizeof(g_overlay));
#endif
    pj64_log_close();
}

void PJ64_CALL GetDllInfo(PLUGIN_INFO *plugin_info)
{
    if (!plugin_info) {
        return;
    }

    memset(plugin_info, 0, sizeof(*plugin_info));
    plugin_info->Version = PLUGIN_VERSION;
    plugin_info->Type = PLUGIN_TYPE_GFX;
    snprintf(plugin_info->Name, sizeof(plugin_info->Name), "SoftRDP");
    plugin_info->NormalMemory = 1;
    plugin_info->MemoryBswaped = 1;
}

void PJ64_CALL CaptureScreen(char *directory)
{
    (void)directory;
}

void PJ64_CALL ChangeWindow(void)
{
}

BOOL PJ64_CALL InitiateGFX(GFX_INFO gfx_info)
{
    g_gfx = gfx_info;
    return TRUE;
}

void PJ64_CALL CloseDLL(void)
{
    stop_runtime();
    memset(&g_gfx, 0, sizeof(g_gfx));
}

void PJ64_CALL RomOpen(void)
{
    (void)start_runtime();
}

void PJ64_CALL RomClosed(void)
{
    stop_runtime();
}

void PJ64_CALL ProcessDList(void)
{
    /*
     * This is the HLE graphics-list entry point in the PJ64 API. SoftRDP is
     * an LLE RDP plugin, so treating it as an RDP command buffer can make the
     * emulator wait forever on bogus DP state.
     */
    g_process_dlist_calls++;
    if (PJ64_LOG_ENABLED &&
        (g_process_dlist_calls <= 8u || (g_process_dlist_calls % 120u) == 0u)) {
        pj64_log_printf("ProcessDList ignored call=%u", g_process_dlist_calls);
    }
}

void PJ64_CALL ProcessRDPList(void)
{
    sr_result result = SR_ERROR_INVALID_ARGUMENT;
    sr_debug_stats stats;

    (void)start_runtime();
    g_process_rdp_calls++;

    if (g_context) {
        // Detect F11 keypress (edge-triggered) to dump current frame data
        static bool g_f11_was_down = false;
        bool f11_is_down = (GetAsyncKeyState(VK_F11) & 0x8000) != 0;
        if (f11_is_down && !g_f11_was_down) {
            g_dump_requested = true;
            pj64_log_printf("F11 pressed: Requesting frame dump on next RDP list");
        }
        g_f11_was_down = f11_is_down;
 
        if (g_dump_requested) {
            g_dump_requested = false;
            g_record_active = true;
            g_recorded_lists_count = 0;
            g_dump_presents_remaining = FRAME_DUMP_PRESENT_COUNT;
            
            g_frame_dump_file = fopen("frame_dump.bin", "wb");
            if (g_frame_dump_file) {
                uint32_t magic = 0x34444653; // "SFD4"
                uint32_t rdram_size = g_rdram_size;
                uint32_t bswapped = g_gfx.MemoryBswaped ? 1 : 0;
                uint32_t zero = 0;
                uint32_t state_size = (uint32_t)sr_state_snapshot_size();
                void *state = malloc(state_size);
                
                fwrite(&magic, 4, 1, g_frame_dump_file);
                fwrite(&rdram_size, 4, 1, g_frame_dump_file);
                fwrite(&bswapped, 4, 1, g_frame_dump_file);
                g_list_count_offset = ftell(g_frame_dump_file);
                fwrite(&zero, 4, 1, g_frame_dump_file); // list_count placeholder
                fwrite(&state_size, 4, 1, g_frame_dump_file);
                if (state && sr_save_state(g_context, state, state_size) == SR_OK)
                    fwrite(state, 1, state_size, g_frame_dump_file);
                else {
                    pj64_log_printf("ERROR: Could not capture RDP state for frame dump");
                    fclose(g_frame_dump_file);
                    g_frame_dump_file = NULL;
                    g_record_active = false;
                }
                free(state);
                
                // Write initial RDRAM
                if (g_frame_dump_file &&
                    !dump_write_all(g_frame_dump_file, g_gfx.RDRAM, g_rdram_size)) {
                    pj64_log_printf("ERROR: Could not write initial RDRAM to frame dump (errno=%d)", errno);
                    fclose(g_frame_dump_file);
                    g_frame_dump_file = NULL;
                    g_record_active = false;
                }
                free(g_dump_rdram_shadow);
                g_dump_rdram_shadow = NULL;
                if (g_frame_dump_file) {
                    g_dump_rdram_shadow = malloc(g_rdram_size);
                    if (g_dump_rdram_shadow)
                        memcpy(g_dump_rdram_shadow, g_gfx.RDRAM, g_rdram_size);
                    else {
                        fclose(g_frame_dump_file);
                        g_frame_dump_file = NULL;
                        g_record_active = false;
                    }
                }
                
                pj64_log_printf("Starting frame dump recording for %u presented frames...",
                                  FRAME_DUMP_PRESENT_COUNT);
            } else {
                g_record_active = false;
                pj64_log_printf("ERROR: Could not open frame_dump.bin for writing!");
            }
        }
 
        if (g_record_active && g_frame_dump_file) {
            uint32_t current = read_reg_value(g_gfx.DPC_CURRENT_REG) & ~7u;
            uint32_t end = read_reg_value(g_gfx.DPC_END_REG) & ~7u;
            uint32_t size = (end > current && current < g_rdram_size && end <= g_rdram_size) ? (end - current) : 0;
            
            uint32_t dp_regs[8];
            dp_regs[0] = read_reg_value(g_gfx.DPC_START_REG);
            dp_regs[1] = read_reg_value(g_gfx.DPC_END_REG);
            dp_regs[2] = read_reg_value(g_gfx.DPC_CURRENT_REG);
            dp_regs[3] = read_reg_value(g_gfx.DPC_STATUS_REG);
            dp_regs[4] = read_reg_value(g_gfx.DPC_CLOCK_REG);
            dp_regs[5] = read_reg_value(g_gfx.DPC_BUFBUSY_REG);
            dp_regs[6] = read_reg_value(g_gfx.DPC_PIPEBUSY_REG);
            dp_regs[7] = read_reg_value(g_gfx.DPC_TMEM_REG);
            
            uint32_t vi_regs[14];
            vi_regs[0] = read_reg_value(g_gfx.VI_STATUS_REG);
            vi_regs[1] = read_reg_value(g_gfx.VI_ORIGIN_REG);
            vi_regs[2] = read_reg_value(g_gfx.VI_WIDTH_REG);
            vi_regs[3] = read_reg_value(g_gfx.VI_INTR_REG);
            vi_regs[4] = read_reg_value(g_gfx.VI_V_CURRENT_LINE_REG);
            vi_regs[5] = read_reg_value(g_gfx.VI_TIMING_REG);
            vi_regs[6] = read_reg_value(g_gfx.VI_V_SYNC_REG);
            vi_regs[7] = read_reg_value(g_gfx.VI_H_SYNC_REG);
            vi_regs[8] = read_reg_value(g_gfx.VI_LEAP_REG);
            vi_regs[9] = read_reg_value(g_gfx.VI_H_START_REG);
            vi_regs[10] = read_reg_value(g_gfx.VI_V_START_REG);
            vi_regs[11] = read_reg_value(g_gfx.VI_V_BURST_REG);
            vi_regs[12] = read_reg_value(g_gfx.VI_X_SCALE_REG);
            vi_regs[13] = read_reg_value(g_gfx.VI_Y_SCALE_REG);
            
            if (!dump_write_rdram_deltas(g_frame_dump_file)) {
                pj64_log_printf("ERROR: Could not write RDRAM deltas to frame dump");
                fclose(g_frame_dump_file);
                g_frame_dump_file = NULL;
                g_record_active = false;
                free(g_dump_rdram_shadow);
                g_dump_rdram_shadow = NULL;
            } else {
                fwrite(&size, 4, 1, g_frame_dump_file);
                fwrite(dp_regs, 4, 8, g_frame_dump_file);
                fwrite(vi_regs, 4, 14, g_frame_dump_file);
                if (size > 0)
                    fwrite(g_gfx.RDRAM + current, 1, size, g_frame_dump_file);
                g_recorded_lists_count++;
            }
        }

        result = sr_process_rdp_list(g_context);
        stats = sr_get_debug_stats(g_context);
        if (PJ64_LOG_ENABLED &&
            (log_sample_u32(g_process_rdp_calls, 32u, 600u) ||
             (result != SR_OK && log_sample_u32(g_process_rdp_calls, 128u, 120u)))) {
            pj64_log_printf("RDP call=%u result=%s list=%08x-%08x bytes=%u last=%08x %02x/%s ci=%08x/%ux%u/%u",
                              g_process_rdp_calls,
                              result_name(result),
                              stats.last_list_current,
                              stats.last_list_end,
                              stats.last_list_bytes,
                              stats.last_command_address,
                              stats.last_command_id,
                              rdp_command_name((rdp_command_id)stats.last_command_id),
                              stats.color_image_address,
                              stats.color_image_width,
                              stats.color_image_size,
                              stats.color_image_format);
            if (result != SR_OK) {
                log_texture_debug(&stats);
            } else if (command_has_texture_debug(stats.last_command_id)) {
                log_texture_debug(&stats);
            }
        }
    } else {
        pj64_log_printf("RDP call=%u no context, acknowledging", g_process_rdp_calls);
        acknowledge_dp_list();
    }
}

void PJ64_CALL DrawScreen(void)
{
#if SOFTRDP_ENABLE_PERF_LOG
    LARGE_INTEGER start, end, freq;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&start);
#endif
    sr_present_draw(&g_present);
#if SOFTRDP_ENABLE_PERF_LOG
    QueryPerformanceCounter(&end);
    g_perf_draw_total_ms += (double)(end.QuadPart - start.QuadPart) * 1000.0 / (double)freq.QuadPart;
#endif
}

void PJ64_CALL ReadScreen(void **dest, long *width, long *height)
{
    if (dest) {
        *dest = NULL;
    }
    if (width) {
        *width = 0;
    }
    if (height) {
        *height = 0;
    }
}

void PJ64_CALL UpdateScreen(void)
{
    sr_vi_frame_info vi_info;
    sr_framebuffer fb;
    sr_result result;
    bool uploaded = false;
#if SOFTRDP_ENABLE_LOG
    uint32_t scanout_nonblack = 0;
    uint32_t scanout_xor = 0;
#endif

    ensure_window_size(g_gfx.hWnd);

    (void)start_runtime();
    g_update_screen_calls++;
    memset(&vi_info, 0, sizeof(vi_info));
    if (g_context) (void)sr_get_vi_frame_info(g_context, &vi_info);
    if (vi_info.width && vi_info.height &&
        vi_info.width <= PJ64_MAX_FRAME_WIDTH &&
        vi_info.height <= PJ64_MAX_FRAME_HEIGHT) {
        g_frame_width = vi_info.width;
        g_frame_height = vi_info.height;
        g_display_width = vi_info.display_width ? vi_info.display_width : vi_info.width;
        g_display_height = vi_info.display_height ? vi_info.display_height : vi_info.height;
    }
    sr_present_set_display_size(&g_present, g_display_width, g_display_height);

    if (g_record_active && g_dump_presents_remaining > 0u) {
        g_dump_presents_remaining--;
    }

    if (g_record_active && g_dump_presents_remaining == 0u) {
        g_record_active = false;
        if (g_frame_dump_file) {
            // Write final reference RDRAM output
            const bool wrote_rdram = dump_write_all(g_frame_dump_file, g_gfx.RDRAM,
                                                     g_rdram_size);
            const bool sought_header = wrote_rdram &&
                                       fseek(g_frame_dump_file, g_list_count_offset, SEEK_SET) == 0;
            const bool wrote_count = sought_header &&
                                     fwrite(&g_recorded_lists_count, 4, 1, g_frame_dump_file) == 1;
            const bool closed = fclose(g_frame_dump_file) == 0;
            g_frame_dump_file = NULL;
            free(g_dump_rdram_shadow);
            g_dump_rdram_shadow = NULL;
            if (wrote_rdram && wrote_count && closed)
                pj64_log_printf("Frame dump completed successfully! Recorded %u lists.", g_recorded_lists_count);
            else
                pj64_log_printf("ERROR: Frame dump incomplete (rdram=%d header=%d close=%d errno=%d)",
                                  wrote_rdram ? 1 : 0, wrote_count ? 1 : 0,
                                  closed ? 1 : 0, errno);
        }
    }

    if (!g_context || !g_present.ready || !ensure_frame_storage(g_frame_width, g_frame_height)) {
        if (PJ64_LOG_ENABLED && log_sample_u32(g_update_screen_calls, 32u, 600u)) {
            pj64_log_printf("UpdateScreen call=%u skipped context=%d present=%d target=%ux%u stride=%u",
                              g_update_screen_calls,
                              g_context ? 1 : 0,
                              g_present.ready ? 1 : 0,
                              g_frame_width,
                              g_frame_height,
                              g_frame_width);
        }
        return;
    }

    memset(&fb, 0, sizeof(fb));
    fb.pixels = g_frame_pixels;
    fb.width = g_frame_width;
    fb.height = g_frame_height;
    fb.stride_pixels = g_frame_width;

    result = sr_update_screen(g_context, &fb);
#if SOFTRDP_ENABLE_PERF_OVERLAY
    prepare_perf_overlay();
    LARGE_INTEGER present_start, present_end;
    QueryPerformanceCounter(&present_start);
#endif
    if (result == SR_OK && fb.valid && vi_info.display) {
#if SOFTRDP_ENABLE_LOG
        for (uint32_t y = 0; y < fb.height; y++) {
            const sr_rgba8 *row = fb.pixels + y * fb.stride_pixels;
            for (uint32_t x = 0; x < fb.width; x++) {
                const uint32_t packed = ((uint32_t)row[x].r << 24) |
                                        ((uint32_t)row[x].g << 16) |
                                        ((uint32_t)row[x].b << 8) | row[x].a;
                scanout_xor ^= packed;
                if (row[x].r || row[x].g || row[x].b) scanout_nonblack++;
            }
        }
#endif
        uploaded = sr_present_upload_rgba8(&g_present, fb.pixels, fb.width, fb.height, fb.stride_pixels);
    } else {
        /* VI scanout is authoritative. Never expose raw RDRAM as a fallback;
         * invalid VI state and scanout errors both produce a black frame. */
        memset(g_frame_pixels, 0,
               g_frame_width * g_frame_height * sizeof(*g_frame_pixels));
        uploaded = sr_present_upload_rgba8(&g_present, g_frame_pixels,
                                           g_frame_width, g_frame_height,
                                           g_frame_width);
    }
#if SOFTRDP_ENABLE_PERF_OVERLAY
    QueryPerformanceCounter(&present_end);
    record_present_time(present_start, present_end);
#endif

#if SOFTRDP_ENABLE_PERF_LOG
    g_perf_frame_count++;
    if (g_perf_frame_count >= 60) {
        log_perf_summary("Periodic");
    }
#endif

    if (uploaded) {
        g_uploaded_frames++;
    }

#if SOFTRDP_ENABLE_LOG
    if (PJ64_LOG_ENABLED &&
        (log_sample_u32(g_update_screen_calls, 32u, 600u) ||
         (!uploaded && log_sample_u32(g_update_screen_calls, 128u, 120u)))) {
        pj64_log_printf("UpdateScreen call=%u result=%s fb_valid=%d uploaded=%d vi_status=%08x origin=%08x width=%u target=%ux%u stride=%u nonblack=%u scanout_xor=%08x frames=%u",
                          g_update_screen_calls,
                          result_name(result),
                          fb.valid ? 1 : 0,
                          uploaded ? 1 : 0,
                          read_reg_value(g_gfx.VI_STATUS_REG),
                          read_reg_value(g_gfx.VI_ORIGIN_REG),
                          read_reg_value(g_gfx.VI_WIDTH_REG),
                          g_frame_width,
                          g_frame_height,
                          g_frame_width,
                          scanout_nonblack,
                          scanout_xor,
                          g_uploaded_frames);
    }
#endif
}

void PJ64_CALL ViStatusChanged(void)
{
}

void PJ64_CALL ViWidthChanged(void)
{
}

void PJ64_CALL ShowCFB(void)
{
}

void PJ64_CALL MoveScreen(int xpos, int ypos)
{
    (void)xpos;
    (void)ypos;
}

void PJ64_CALL FBWrite(DWORD addr, DWORD size)
{
    (void)addr;
    (void)size;
}

void PJ64_CALL FBWList(FrameBufferModifyEntry *plist, DWORD size)
{
    (void)plist;
    (void)size;
}

void PJ64_CALL FBRead(DWORD addr)
{
    (void)addr;
}

void PJ64_CALL FBGetFrameBufferInfo(void *pinfo)
{
    (void)pinfo;
}

void PJ64_CALL DllAbout(HWND hwnd)
{
    (void)hwnd;
}

void PJ64_CALL DllConfig(HWND hwnd)
{
    (void)hwnd;
}

void PJ64_CALL DllTest(HWND hwnd)
{
    (void)hwnd;
}
