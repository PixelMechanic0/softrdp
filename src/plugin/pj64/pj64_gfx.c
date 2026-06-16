#include "pj64_gfx.h"
#include "pj64_log.h"

#include "../../core/rdp_commands.h"
#include "../../core/sr.h"
#include "../../present/sr_present.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PJ64_MAX_FRAME_WIDTH 800u
#define PJ64_MAX_FRAME_HEIGHT 600u

typedef struct vi_target {
    uint32_t width;
    uint32_t height;
    uint32_t stride_pixels;
} vi_target;

static GFX_INFO g_gfx;
static sr_context *g_context;
static sr_present g_present;
static sr_rgba8 *g_frame_pixels;
static uint32_t g_frame_capacity_pixels;
static bool g_runtime_started;
static uint32_t g_process_dlist_calls;
static uint32_t g_process_rdp_calls;
static uint32_t g_update_screen_calls;
static uint32_t g_uploaded_frames;
static uint64_t g_last_logged_commands;
static uint64_t g_last_logged_draws;

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

static uint32_t clamp_u32(uint32_t value, uint32_t max)
{
    return value > max ? max : value;
}

static vi_target make_vi_target(void)
{
    const uint32_t vi_width = read_reg_value(g_gfx.VI_WIDTH_REG) & 0xfffu;
    const uint32_t h_start = read_reg_value(g_gfx.VI_H_START_REG);
    const uint32_t v_start = read_reg_value(g_gfx.VI_V_START_REG);
    const uint32_t x_scale = read_reg_value(g_gfx.VI_X_SCALE_REG);
    const uint32_t y_scale = read_reg_value(g_gfx.VI_Y_SCALE_REG);
    const int32_t h0 = (int32_t)((h_start >> 16) & 0x3ffu);
    const int32_t h1 = (int32_t)(h_start & 0x3ffu);
    const uint32_t y0 = (v_start >> 16) & 0x3ffu;
    const uint32_t y1 = v_start & 0x3ffu;
    const uint32_t x_add = x_scale & 0xfffu;
    const uint32_t y_add = y_scale & 0xfffu;
    const uint32_t visible_h = h1 > h0 ? (uint32_t)(h1 - h0) : 0u;
    const uint32_t visible_v = y1 > y0 ? (y1 - y0) >> 1 : 0u;
    vi_target target;

    memset(&target, 0, sizeof(target));
    target.stride_pixels = vi_width;

    if (visible_h && x_add) {
        target.width = visible_h * x_add / 1024u;
    }
    if (visible_v && y_add) {
        target.height = visible_v * y_add / 1024u;
    }

    if (target.width == 0 || target.width > vi_width) {
        target.width = vi_width;
    }
    if (target.height == 0) {
        target.height = visible_v ? visible_v : 240u;
    }

    target.width = clamp_u32(target.width, PJ64_MAX_FRAME_WIDTH);
    target.height = clamp_u32(target.height, PJ64_MAX_FRAME_HEIGHT);
    return target;
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

static uint32_t rdram_byte_index(uint32_t addr)
{
    return g_gfx.MemoryBswaped ? (addr ^ 3u) : addr;
}

static bool read_rdram_u8(uint32_t addr, uint8_t *value)
{
    if (!g_gfx.RDRAM || !value || addr >= SR_RDRAM_MAX_SIZE) {
        return false;
    }

    *value = g_gfx.RDRAM[rdram_byte_index(addr)];
    return true;
}

static bool read_rdram_be16(uint32_t addr, uint16_t *value)
{
    uint8_t hi;
    uint8_t lo;

    if (addr + 1u >= SR_RDRAM_MAX_SIZE ||
        !read_rdram_u8(addr, &hi) || !read_rdram_u8(addr + 1u, &lo)) {
        return false;
    }

    *value = ((uint16_t)hi << 8) | lo;
    return true;
}

static bool read_rdram_be32(uint32_t addr, uint32_t *value)
{
    uint8_t b0;
    uint8_t b1;
    uint8_t b2;
    uint8_t b3;

    if (addr + 3u >= SR_RDRAM_MAX_SIZE ||
        !read_rdram_u8(addr, &b0) || !read_rdram_u8(addr + 1u, &b1) ||
        !read_rdram_u8(addr + 2u, &b2) || !read_rdram_u8(addr + 3u, &b3)) {
        return false;
    }

    *value = ((uint32_t)b0 << 24) | ((uint32_t)b1 << 16) | ((uint32_t)b2 << 8) | b3;
    return true;
}

static sr_rgba8 rgba5551_to_rgba8(uint16_t value)
{
    sr_rgba8 color;
    color.r = (uint8_t)(((value >> 11) & 0x1fu) * 255u / 31u);
    color.g = (uint8_t)(((value >> 6) & 0x1fu) * 255u / 31u);
    color.b = (uint8_t)(((value >> 1) & 0x1fu) * 255u / 31u);
    color.a = (value & 1u) ? 0xffu : 0u;
    return color;
}

static bool upload_raw_cfb(vi_target target)
{
    const uint32_t origin = read_reg_value(g_gfx.VI_ORIGIN_REG) & 0x00ffffffu;
    const uint32_t type = read_reg_value(g_gfx.VI_STATUS_REG) & 3u;

    if (origin == 0 || target.width == 0 || target.height == 0 ||
        target.stride_pixels == 0 || target.width > target.stride_pixels ||
        target.width > PJ64_MAX_FRAME_WIDTH || target.height > PJ64_MAX_FRAME_HEIGHT ||
        (type != 2u && type != 3u) || !ensure_frame_storage(target.width, target.height)) {
        return false;
    }

    for (uint32_t y = 0; y < target.height; y++) {
        for (uint32_t x = 0; x < target.width; x++) {
            const uint32_t src_pixel = y * target.stride_pixels + x;
            const uint32_t dst_pixel = y * target.width + x;

            if (type == 3u) {
                uint32_t raw;
                if (!read_rdram_be32(origin + src_pixel * 4u, &raw)) {
                    return false;
                }
                g_frame_pixels[dst_pixel] = (sr_rgba8){
                    (uint8_t)(raw >> 24),
                    (uint8_t)(raw >> 16),
                    (uint8_t)(raw >> 8),
                    (uint8_t)raw
                };
            } else {
                uint16_t raw;
                if (!read_rdram_be16(origin + src_pixel * 2u, &raw)) {
                    return false;
                }
                g_frame_pixels[dst_pixel] = rgba5551_to_rgba8(raw);
            }
        }
    }

    return sr_present_upload_rgba8(&g_present, g_frame_pixels, target.width, target.height, target.width);
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

static bool log_sample_u32(uint32_t count, uint32_t early_count, uint32_t interval)
{
    return count <= early_count || (interval != 0u && (count % interval) == 0u);
}

static sr_host_interface make_host_interface(GFX_INFO *gfx)
{
    sr_host_interface host;
    memset(&host, 0, sizeof(host));

    host.rdram = gfx->RDRAM;
    host.rdram_size = SR_RDRAM_MAX_SIZE;
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
    return true;
}

static void stop_runtime(void)
{
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
    g_last_logged_commands = 0;
    g_last_logged_draws = 0;
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
    if (g_process_dlist_calls <= 8u || (g_process_dlist_calls % 120u) == 0u) {
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
        result = sr_process_rdp_list(g_context);
        stats = sr_get_debug_stats(g_context);
        if (log_sample_u32(g_process_rdp_calls, 32u, 600u) ||
            (result != SR_OK && log_sample_u32(g_process_rdp_calls, 128u, 120u)) ||
            stats.draw_calls_seen != g_last_logged_draws) {
            pj64_log_printf("RDP call=%u result=%s list=%08x-%08x bytes=%u last=%08x %02x/%s commands=%llu draws=%llu",
                              g_process_rdp_calls,
                              result_name(result),
                              stats.last_list_current,
                              stats.last_list_end,
                              stats.last_list_bytes,
                              stats.last_command_address,
                              stats.last_command_id,
                              rdp_command_name((rdp_command_id)stats.last_command_id),
                              (unsigned long long)stats.commands_seen,
                              (unsigned long long)stats.draw_calls_seen);
            g_last_logged_commands = stats.commands_seen;
            g_last_logged_draws = stats.draw_calls_seen;
        }
    } else {
        pj64_log_printf("RDP call=%u no context, acknowledging", g_process_rdp_calls);
        acknowledge_dp_list();
    }
}

void PJ64_CALL DrawScreen(void)
{
    sr_present_draw(&g_present);
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
    const vi_target target = make_vi_target();
    sr_framebuffer fb;
    sr_result result;
    bool uploaded = false;

    ensure_window_size(g_gfx.hWnd);

    (void)start_runtime();
    g_update_screen_calls++;

    if (!g_context || !g_present.ready || target.width == 0 ||
        target.height == 0 || !ensure_frame_storage(target.width, target.height)) {
        if (log_sample_u32(g_update_screen_calls, 32u, 600u)) {
            pj64_log_printf("UpdateScreen call=%u skipped context=%d present=%d target=%ux%u stride=%u",
                              g_update_screen_calls,
                              g_context ? 1 : 0,
                              g_present.ready ? 1 : 0,
                              target.width,
                              target.height,
                              target.stride_pixels);
        }
        return;
    }

    memset(&fb, 0, sizeof(fb));
    fb.pixels = g_frame_pixels;
    fb.width = target.width;
    fb.height = target.height;
    fb.stride_pixels = target.width;

    result = sr_update_screen(g_context, &fb);
    if (result == SR_OK && fb.valid) {
        uploaded = sr_present_upload_rgba8(&g_present, fb.pixels, fb.width, fb.height, fb.stride_pixels);
    } else {
        uploaded = upload_raw_cfb(target);
    }

    if (uploaded) {
        g_uploaded_frames++;
    }

    if (log_sample_u32(g_update_screen_calls, 32u, 600u) ||
        (!uploaded && log_sample_u32(g_update_screen_calls, 128u, 120u))) {
        pj64_log_printf("UpdateScreen call=%u result=%s fb_valid=%d uploaded=%d vi_status=%08x origin=%08x width=%u target=%ux%u stride=%u frames=%u",
                          g_update_screen_calls,
                          result_name(result),
                          fb.valid ? 1 : 0,
                          uploaded ? 1 : 0,
                          read_reg_value(g_gfx.VI_STATUS_REG),
                          read_reg_value(g_gfx.VI_ORIGIN_REG),
                          read_reg_value(g_gfx.VI_WIDTH_REG),
                          target.width,
                          target.height,
                          target.stride_pixels,
                          g_uploaded_frames);
    }
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
