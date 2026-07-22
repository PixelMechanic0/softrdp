#include "pj64_gfx.h"
#include "pj64_dump.h"
#include "pj64_log.h"

#include "../../core/rdp_commands.h"
#include "../../core/sr.h"
#include "../../present/sr_present.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

static bool g_fullscreen = false;
#ifdef _WIN32
static HMENU g_old_menu = NULL;
static LONG g_old_style = 0;
static WINDOWPLACEMENT g_old_pos;
#endif

static uint32_t g_rdram_size = 0x400000;

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
static uint32_t read_reg_value(DWORD *reg)
{
    return reg ? (uint32_t)*reg : 0;
}

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

#if SOFTRDP_ENABLE_LOG
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
#endif

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
    if (!hwnd || g_fullscreen) {
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

    if (!g_gfx.MemoryBswaped) {
        pj64_log_printf("start_runtime: host did not provide word-swapped RDRAM");
        MessageBoxA(g_gfx.hWnd,
                    "SoftRDP requires word-swapped RDRAM from the emulator.",
                    "SoftRDP",
                    MB_ICONERROR | MB_OK);
        return false;
    }

    host = make_host_interface(&g_gfx);
    pj64_dump_detach();
    sr_destroy(g_context);
    g_context = sr_create(&host);
    if (!g_context) {
        pj64_log_printf("start_runtime: sr_create failed");
        return false;
    }
    pj64_dump_attach(&g_gfx, g_context, g_rdram_size);

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
    return true;
}

static void stop_runtime(void)
{
    if (g_fullscreen) {
        ChangeWindow();
    }
    pj64_dump_detach();
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
#ifdef _WIN32
    HWND hwnd = g_gfx.hWnd;
    if (!hwnd || !IsWindow(hwnd)) {
        return;
    }

    g_fullscreen = !g_fullscreen;

    if (g_fullscreen) {
        // hide cursor
        ShowCursor(FALSE);

        // hide status bar
        if (g_gfx.hStatusBar) {
            ShowWindow(g_gfx.hStatusBar, SW_HIDE);
        }

        // disable menu and save it to restore it later
        g_old_menu = GetMenu(hwnd);
        if (g_old_menu) {
            SetMenu(hwnd, NULL);
        }

        // save old window position and size
        g_old_pos.length = sizeof(g_old_pos);
        GetWindowPlacement(hwnd, &g_old_pos);

        // use virtual screen dimensions for fullscreen mode
        int32_t vs_width = GetSystemMetrics(SM_CXSCREEN);
        int32_t vs_height = GetSystemMetrics(SM_CYSCREEN);

        // disable all styles to get a borderless window and save it to restore it later
        g_old_style = GetWindowLongA(hwnd, GWL_STYLE);
        LONG style = WS_VISIBLE | WS_POPUP;
        SetWindowLongA(hwnd, GWL_STYLE, style);

        // resize window so it covers the entire virtual screen
        SetWindowPos(hwnd, HWND_TOP, 0, 0, vs_width, vs_height, SWP_SHOWWINDOW);
    }
    else {
        // restore cursor
        ShowCursor(TRUE);

        // restore status bar
        if (g_gfx.hStatusBar) {
            ShowWindow(g_gfx.hStatusBar, SW_SHOW);
        }

        // restore menu
        if (g_old_menu) {
            SetMenu(hwnd, g_old_menu);
            g_old_menu = NULL;
        }

        // restore style
        SetWindowLongA(hwnd, GWL_STYLE, g_old_style);

        // restore window size and position
        g_old_pos.length = sizeof(g_old_pos);
        SetWindowPlacement(hwnd, &g_old_pos);
    }
#endif
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
#if SOFTRDP_ENABLE_LOG
    sr_result result = SR_ERROR_INVALID_ARGUMENT;
#endif

    (void)start_runtime();
    g_process_rdp_calls++;

    if (g_context) {
        const bool dump_armed = pj64_dump_armed;
        if (dump_armed) pj64_dump_before_list();
#if SOFTRDP_ENABLE_LOG
        result = sr_process_rdp_list(g_context);
#else
        (void)sr_process_rdp_list(g_context);
#endif
        if (dump_armed) pj64_dump_after_list();
#if SOFTRDP_ENABLE_LOG
        if (PJ64_LOG_ENABLED &&
            (log_sample_u32(g_process_rdp_calls, 32u, 600u) ||
             (result != SR_OK && log_sample_u32(g_process_rdp_calls, 128u, 120u)))) {
            const sr_debug_stats stats = sr_get_debug_stats(g_context);
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
#endif
    } else {
#if SOFTRDP_ENABLE_LOG
        pj64_log_printf("RDP call=%u no context, acknowledging", g_process_rdp_calls);
#endif
        acknowledge_dp_list();
    }
}

void PJ64_CALL DrawScreen(void)
{
    /* UpdateScreen owns presentation. PJ64 may call DrawScreen separately;
     * swapping here as well presents one frame twice and can block twice on
     * the display driver. */
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
    pj64_dump_poll_hotkey();
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

    if (pj64_dump_armed) pj64_dump_on_present();

    if (!g_context || !g_present.ready || !ensure_frame_storage(g_frame_width, g_frame_height)) {
#if SOFTRDP_ENABLE_LOG
        if (PJ64_LOG_ENABLED && log_sample_u32(g_update_screen_calls, 32u, 600u)) {
            pj64_log_printf("UpdateScreen call=%u skipped context=%d present=%d target=%ux%u stride=%u",
                              g_update_screen_calls,
                              g_context ? 1 : 0,
                              g_present.ready ? 1 : 0,
                              g_frame_width,
                              g_frame_height,
                              g_frame_width);
        }
#endif
        return;
    }

    memset(&fb, 0, sizeof(fb));
    fb.pixels = g_frame_pixels;
    fb.width = g_frame_width;
    fb.height = g_frame_height;
    fb.stride_pixels = g_frame_width;

    result = sr_update_screen(g_context, &fb);

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
    } else if (vi_info.hold) {
        /* Valid pixel type but no renderable frame this refresh (e.g. H_START not
         * programmed yet). Hold the last frame instead of flashing black: redraw
         * the retained texture without uploading new pixels. */
        sr_present_draw(&g_present);
        uploaded = false;
    } else {
        /* VI scanout is authoritative. Never expose raw RDRAM as a fallback;
         * a genuine blank signal and scanout errors produce a black frame. */
        memset(g_frame_pixels, 0,
               g_frame_width * g_frame_height * sizeof(*g_frame_pixels));
        uploaded = sr_present_upload_rgba8(&g_present, g_frame_pixels,
                                           g_frame_width, g_frame_height,
                                           g_frame_width);
    }
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

#ifdef HAS_CONFIG
void PJ64_CALL DllConfig(HWND hwnd)
{
    (void)hwnd;
}
#endif

void PJ64_CALL DllTest(HWND hwnd)
{
    (void)hwnd;
}
