#define M64P_PLUGIN_PROTOTYPES 1

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>

#ifdef _WIN32
#include <windows.h>
#define DLSYM(a, b) GetProcAddress(a, b)
#else
#include <dlfcn.h>
#define DLSYM(a, b) dlsym(a, b)
typedef void *HWND;
#endif

#include "api/m64p_types.h"
#include "api/m64p_config.h"
#include "api/m64p_plugin.h"
#include "api/m64p_vidext.h"

#include "../../core/sr.h"
#include "../../present/sr_present.h"

#define M64P_MAX_FRAME_WIDTH 640u
#define M64P_MAX_FRAME_HEIGHT 576u
#define PLUGIN_VERSION              0x010600
#define VIDEO_PLUGIN_API_VERSION    0x020500

static m64p_dynlib_handle CoreLibHandle = NULL;
static void (*debug_callback)(void *, int, const char *) = NULL;
static void *debug_call_context = NULL;
static void (*render_callback)(int) = NULL;

static ptr_ConfigOpenSection      ConfigOpenSection = NULL;
static ptr_ConfigSaveSection      ConfigSaveSection = NULL;
static ptr_ConfigSetDefaultInt    ConfigSetDefaultInt = NULL;
static ptr_ConfigSetDefaultBool   ConfigSetDefaultBool = NULL;
static ptr_ConfigGetParamInt      ConfigGetParamInt = NULL;
static ptr_ConfigGetParamBool     ConfigGetParamBool = NULL;

static ptr_VidExt_Init                  CoreVideo_Init = NULL;
static ptr_VidExt_Quit                  CoreVideo_Quit = NULL;
static ptr_VidExt_SetVideoMode          CoreVideo_SetVideoMode = NULL;
static ptr_VidExt_SetCaption            CoreVideo_SetCaption = NULL;
static ptr_VidExt_ToggleFullScreen      CoreVideo_ToggleFullScreen = NULL;
static ptr_VidExt_ResizeWindow          CoreVideo_ResizeWindow = NULL;
static ptr_VidExt_GL_GetProcAddress     CoreVideo_GL_GetProcAddress = NULL;
static ptr_VidExt_GL_SetAttribute       CoreVideo_GL_SetAttribute = NULL;
static ptr_VidExt_GL_SwapBuffers        CoreVideo_GL_SwapBuffers = NULL;

static GFX_INFO g_gfx;
static sr_context *g_context = NULL;
static sr_present g_present;

static int32_t g_win_width = 640;
static int32_t g_win_height = 480;
static bool g_win_fullscreen = false;
static bool g_plugin_initialized = false;

static sr_rgba8 *g_frame_pixels = NULL;
static uint32_t g_frame_capacity_pixels = 0;
static uint32_t g_frame_width = 0;
static uint32_t g_frame_height = 0;

static void msg_log(int level, const char *fmt, ...)
{
    if (!debug_callback) {
        return;
    }
    char buf[1024];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    debug_callback(debug_call_context, level, buf);
}

static bool ensure_frame_storage(uint32_t width, uint32_t height)
{
    const uint32_t pixels = width * height;
    sr_rgba8 *new_pixels;

    if (width == 0 || height == 0 || width > M64P_MAX_FRAME_WIDTH || height > M64P_MAX_FRAME_HEIGHT) {
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

static void *mupen_gl_proc_loader(const char *name)
{
    if (CoreVideo_GL_GetProcAddress) {
        return (void *)CoreVideo_GL_GetProcAddress(name);
    }
    return NULL;
}

EXPORT m64p_error CALL PluginStartup(m64p_dynlib_handle _CoreLibHandle, void *Context,
                                     void (*DebugCallback)(void *, int, const char *))
{
    if (g_plugin_initialized) {
        return M64ERR_ALREADY_INIT;
    }

    debug_callback = DebugCallback;
    debug_call_context = Context;
    CoreLibHandle = _CoreLibHandle;

    ConfigOpenSection = (ptr_ConfigOpenSection)DLSYM(CoreLibHandle, "ConfigOpenSection");
    ConfigSaveSection = (ptr_ConfigSaveSection)DLSYM(CoreLibHandle, "ConfigSaveSection");
    ConfigSetDefaultInt = (ptr_ConfigSetDefaultInt)DLSYM(CoreLibHandle, "ConfigSetDefaultInt");
    ConfigSetDefaultBool = (ptr_ConfigSetDefaultBool)DLSYM(CoreLibHandle, "ConfigSetDefaultBool");
    ConfigGetParamInt = (ptr_ConfigGetParamInt)DLSYM(CoreLibHandle, "ConfigGetParamInt");
    ConfigGetParamBool = (ptr_ConfigGetParamBool)DLSYM(CoreLibHandle, "ConfigGetParamBool");

    m64p_handle configGeneral = NULL;
    if (ConfigOpenSection && ConfigOpenSection("Video-General", &configGeneral) == M64ERR_SUCCESS) {
        if (ConfigSetDefaultBool) {
            ConfigSetDefaultBool(configGeneral, "Fullscreen", 0, "Use fullscreen mode if True, or windowed mode if False");
        }
        if (ConfigSetDefaultInt) {
            ConfigSetDefaultInt(configGeneral, "ScreenWidth", 640, "Width of output window or fullscreen width");
            ConfigSetDefaultInt(configGeneral, "ScreenHeight", 480, "Height of output window or fullscreen height");
        }
        if (ConfigSaveSection) {
            ConfigSaveSection("Video-General");
        }
    }

    g_plugin_initialized = true;
    return M64ERR_SUCCESS;
}

EXPORT m64p_error CALL PluginShutdown(void)
{
    if (!g_plugin_initialized) {
        return M64ERR_NOT_INIT;
    }

    release_frame_storage();
    debug_callback = NULL;
    debug_call_context = NULL;
    g_plugin_initialized = false;
    return M64ERR_SUCCESS;
}

EXPORT m64p_error CALL PluginGetVersion(m64p_plugin_type *PluginType, int *PluginVersion, int *APIVersion, const char **PluginNamePtr, int *Capabilities)
{
    if (PluginType != NULL) {
        *PluginType = M64PLUGIN_GFX;
    }

    if (PluginVersion != NULL) {
        *PluginVersion = PLUGIN_VERSION;
    }

    if (APIVersion != NULL) {
        *APIVersion = VIDEO_PLUGIN_API_VERSION;
    }

    if (PluginNamePtr != NULL) {
        *PluginNamePtr = "SoftRDP-Mupen64Plus";
    }

    if (Capabilities != NULL) {
        *Capabilities = 0;
    }

    return M64ERR_SUCCESS;
}

EXPORT int CALL InitiateGFX(GFX_INFO Gfx_Info)
{
    g_gfx = Gfx_Info;
    return 1;
}

EXPORT void CALL MoveScreen(int xpos, int ypos)
{
    (void)xpos;
    (void)ypos;
}

EXPORT void CALL ProcessDList(void)
{
    /* HLE DList processing is not supported by SoftRDP. RSP must be run in LLE mode. */
}

EXPORT void CALL ProcessRDPList(void)
{
    if (g_context) {
        sr_process_rdp_list(g_context);
    }
}

static void raise_mi_interrupt(void *userdata)
{
    (void)userdata;
    if (g_gfx.CheckInterrupts) {
        g_gfx.CheckInterrupts();
    }
}

EXPORT int CALL RomOpen(void)
{
    CoreVideo_Init = (ptr_VidExt_Init)DLSYM(CoreLibHandle, "VidExt_Init");
    CoreVideo_Quit = (ptr_VidExt_Quit)DLSYM(CoreLibHandle, "VidExt_Quit");
    CoreVideo_SetVideoMode = (ptr_VidExt_SetVideoMode)DLSYM(CoreLibHandle, "VidExt_SetVideoMode");
    CoreVideo_SetCaption = (ptr_VidExt_SetCaption)DLSYM(CoreLibHandle, "VidExt_SetCaption");
    CoreVideo_ToggleFullScreen = (ptr_VidExt_ToggleFullScreen)DLSYM(CoreLibHandle, "VidExt_ToggleFullScreen");
    CoreVideo_ResizeWindow = (ptr_VidExt_ResizeWindow)DLSYM(CoreLibHandle, "VidExt_ResizeWindow");
    CoreVideo_GL_GetProcAddress = (ptr_VidExt_GL_GetProcAddress)DLSYM(CoreLibHandle, "VidExt_GL_GetProcAddress");
    CoreVideo_GL_SetAttribute = (ptr_VidExt_GL_SetAttribute)DLSYM(CoreLibHandle, "VidExt_GL_SetAttribute");
    CoreVideo_GL_SwapBuffers = (ptr_VidExt_GL_SwapBuffers)DLSYM(CoreLibHandle, "VidExt_GL_SwapBuffers");

    if (!CoreVideo_Init || !CoreVideo_Quit || !CoreVideo_SetVideoMode || !CoreVideo_SetCaption ||
        !CoreVideo_ToggleFullScreen || !CoreVideo_GL_GetProcAddress || !CoreVideo_GL_SetAttribute || !CoreVideo_GL_SwapBuffers) {
        msg_log(M64MSG_ERROR, "SoftRDP: Failed to load Mupen64Plus Video Extension functions.");
        return 0;
    }

    m64p_handle configSection = NULL;
    if (ConfigOpenSection && ConfigOpenSection("Video-General", &configSection) == M64ERR_SUCCESS) {
        g_win_fullscreen = ConfigGetParamBool(configSection, "Fullscreen");
        g_win_width = ConfigGetParamInt(configSection, "ScreenWidth");
        g_win_height = ConfigGetParamInt(configSection, "ScreenHeight");
    }
    if (g_win_width <= 0) g_win_width = 640;
    if (g_win_height <= 0) g_win_height = 480;

    if (CoreVideo_Init() != M64ERR_SUCCESS) {
        msg_log(M64MSG_ERROR, "SoftRDP: VidExt_Init failed.");
        return 0;
    }
    CoreVideo_SetCaption("SoftRDP");

    CoreVideo_GL_SetAttribute(M64P_GL_CONTEXT_PROFILE_MASK, M64P_GL_CONTEXT_PROFILE_CORE);
    CoreVideo_GL_SetAttribute(M64P_GL_CONTEXT_MAJOR_VERSION, 3);
    CoreVideo_GL_SetAttribute(M64P_GL_CONTEXT_MINOR_VERSION, 3);
    CoreVideo_GL_SetAttribute(M64P_GL_DOUBLEBUFFER, 1);

    m64p_video_mode mode_flag = g_win_fullscreen ? M64VIDEO_FULLSCREEN : M64VIDEO_WINDOWED;
    if (CoreVideo_SetVideoMode(g_win_width, g_win_height, 0, mode_flag, M64VIDEOFLAG_SUPPORT_RESIZING) != M64ERR_SUCCESS) {
        msg_log(M64MSG_ERROR, "SoftRDP: VidExt_SetVideoMode failed.");
        CoreVideo_Quit();
        return 0;
    }

    sr_host_interface host;
    memset(&host, 0, sizeof(host));
    host.rdram = g_gfx.RDRAM;
    
    uint32_t rdram_size = 0x800000;
    if (g_gfx.version >= 2 && g_gfx.RDRAM_SIZE) {
        rdram_size = *g_gfx.RDRAM_SIZE;
    }
    host.rdram_size = rdram_size;
    host.dmem = g_gfx.DMEM;
    host.mi_intr_reg = (uint32_t *)g_gfx.MI_INTR_REG;
    host.raise_mi_interrupt = raise_mi_interrupt;
    host.userdata = NULL;

    host.dp_regs[SR_DP_START] = (uint32_t *)g_gfx.DPC_START_REG;
    host.dp_regs[SR_DP_END] = (uint32_t *)g_gfx.DPC_END_REG;
    host.dp_regs[SR_DP_CURRENT] = (uint32_t *)g_gfx.DPC_CURRENT_REG;
    host.dp_regs[SR_DP_STATUS] = (uint32_t *)g_gfx.DPC_STATUS_REG;
    host.dp_regs[SR_DP_CLOCK] = (uint32_t *)g_gfx.DPC_CLOCK_REG;
    host.dp_regs[SR_DP_BUFBUSY] = (uint32_t *)g_gfx.DPC_BUFBUSY_REG;
    host.dp_regs[SR_DP_PIPEBUSY] = (uint32_t *)g_gfx.DPC_PIPEBUSY_REG;
    host.dp_regs[SR_DP_TMEM] = (uint32_t *)g_gfx.DPC_TMEM_REG;

    host.vi_regs[SR_VI_STATUS] = (uint32_t *)g_gfx.VI_STATUS_REG;
    host.vi_regs[SR_VI_ORIGIN] = (uint32_t *)g_gfx.VI_ORIGIN_REG;
    host.vi_regs[SR_VI_WIDTH] = (uint32_t *)g_gfx.VI_WIDTH_REG;
    host.vi_regs[SR_VI_INTR] = (uint32_t *)g_gfx.VI_INTR_REG;
    host.vi_regs[SR_VI_CURRENT] = (uint32_t *)g_gfx.VI_V_CURRENT_LINE_REG;
    host.vi_regs[SR_VI_TIMING] = (uint32_t *)g_gfx.VI_TIMING_REG;
    host.vi_regs[SR_VI_V_SYNC] = (uint32_t *)g_gfx.VI_V_SYNC_REG;
    host.vi_regs[SR_VI_H_SYNC] = (uint32_t *)g_gfx.VI_H_SYNC_REG;
    host.vi_regs[SR_VI_LEAP] = (uint32_t *)g_gfx.VI_LEAP_REG;
    host.vi_regs[SR_VI_H_START] = (uint32_t *)g_gfx.VI_H_START_REG;
    host.vi_regs[SR_VI_V_START] = (uint32_t *)g_gfx.VI_V_START_REG;
    host.vi_regs[SR_VI_V_BURST] = (uint32_t *)g_gfx.VI_V_BURST_REG;
    host.vi_regs[SR_VI_X_SCALE] = (uint32_t *)g_gfx.VI_X_SCALE_REG;
    host.vi_regs[SR_VI_Y_SCALE] = (uint32_t *)g_gfx.VI_Y_SCALE_REG;

    g_context = sr_create(&host);
    if (!g_context) {
        msg_log(M64MSG_ERROR, "SoftRDP: Failed to create SoftRDP context.");
        CoreVideo_Quit();
        return 0;
    }

    if (!sr_present_init_external(&g_present, mupen_gl_proc_loader)) {
        msg_log(M64MSG_ERROR, "SoftRDP: Failed to initialize OpenGL presenter.");
        sr_destroy(g_context);
        g_context = NULL;
        CoreVideo_Quit();
        return 0;
    }

    sr_present_set_window_size(&g_present, (uint32_t)g_win_width, (uint32_t)g_win_height);
    sr_present_set_display_size(&g_present, 640, 480);
    sr_present_clear(&g_present);

    return 1;
}

EXPORT void CALL RomClosed(void)
{
    sr_present_shutdown(&g_present);
    if (g_context) {
        sr_destroy(g_context);
        g_context = NULL;
    }
    if (CoreVideo_Quit) {
        CoreVideo_Quit();
    }
}

EXPORT void CALL ShowCFB(void)
{
}

EXPORT void CALL UpdateScreen(void)
{
    bool valid = false;
    if (g_context) {
        sr_vi_frame_info vi_info;
        if (sr_get_vi_frame_info(g_context, &vi_info) == SR_OK && vi_info.display) {
            if (ensure_frame_storage(vi_info.width, vi_info.height)) {
                sr_framebuffer out_fb;
                out_fb.pixels = g_frame_pixels;
                out_fb.width = vi_info.width;
                out_fb.height = vi_info.height;
                out_fb.stride_pixels = vi_info.width;
                out_fb.valid = false;

                if (sr_update_screen(g_context, &out_fb) == SR_OK && out_fb.valid) {
                    g_frame_width = out_fb.width;
                    g_frame_height = out_fb.height;
                    uint32_t display_width = vi_info.display_width ? vi_info.display_width : vi_info.width;
                    uint32_t display_height = vi_info.display_height ? vi_info.display_height : vi_info.height;
                    sr_present_set_window_size(&g_present, (uint32_t)g_win_width, (uint32_t)g_win_height);
                    sr_present_set_display_size(&g_present, display_width, display_height);
                    sr_present_upload_rgba8(&g_present, g_frame_pixels, out_fb.width, out_fb.height, out_fb.stride_pixels);
                    valid = true;
                }
            }
        }
    }

    if (!valid) {
        sr_present_clear(&g_present);
    }

    if (render_callback) {
        render_callback(1);
    }

    if (CoreVideo_GL_SwapBuffers) {
        CoreVideo_GL_SwapBuffers();
    }
}

EXPORT void CALL ViStatusChanged(void)
{
}

EXPORT void CALL ViWidthChanged(void)
{
}

EXPORT void CALL ChangeWindow(void)
{
    if (CoreVideo_ToggleFullScreen) {
        CoreVideo_ToggleFullScreen();
    }
}

EXPORT void CALL ReadScreen2(void *dest, int *width, int *height, int front)
{
    (void)front;
    if (g_frame_pixels && dest && width && height) {
        *width = (int)g_frame_width;
        *height = (int)g_frame_height;
        uint8_t *dst = (uint8_t *)dest;
        const uint32_t total = g_frame_width * g_frame_height;
        for (uint32_t i = 0; i < total; i++) {
            dst[i * 3 + 0] = g_frame_pixels[i].r;
            dst[i * 3 + 1] = g_frame_pixels[i].g;
            dst[i * 3 + 2] = g_frame_pixels[i].b;
        }
    }
}

EXPORT void CALL SetRenderingCallback(void (*callback)(int))
{
    render_callback = callback;
}

EXPORT void CALL ResizeVideoOutput(int width, int height)
{
    g_win_width = width;
    g_win_height = height;
    sr_present_set_window_size(&g_present, (uint32_t)width, (uint32_t)height);
}

EXPORT void CALL FBWrite(unsigned int addr, unsigned int size)
{
    (void)addr;
    (void)size;
}

EXPORT void CALL FBRead(unsigned int addr)
{
    (void)addr;
}

EXPORT void CALL FBGetFrameBufferInfo(void *pinfo)
{
    (void)pinfo;
}
