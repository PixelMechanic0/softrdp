#include "pj64_gfx.h"

#include "../../core/sr.h"

#include <stdio.h>
#include <string.h>

static GFX_INFO g_gfx;
static sr_context *g_context;

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

static sr_host_interface make_host_interface(GFX_INFO *gfx)
{
    sr_host_interface host;
    memset(&host, 0, sizeof(host));

    host.rdram = gfx->RDRAM;
    host.rdram_size = SR_RDRAM_MAX_SIZE;
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
    sr_host_interface host;

    g_gfx = gfx_info;
    host = make_host_interface(&g_gfx);

    sr_destroy(g_context);
    g_context = sr_create(&host);
    return g_context != NULL;
}

void PJ64_CALL CloseDLL(void)
{
    sr_destroy(g_context);
    g_context = NULL;
    memset(&g_gfx, 0, sizeof(g_gfx));
}

void PJ64_CALL RomOpen(void)
{
}

void PJ64_CALL RomClosed(void)
{
    sr_destroy(g_context);
    g_context = NULL;
}

void PJ64_CALL ProcessDList(void)
{
    /*
     * This is the HLE graphics-list entry point in the PJ64 API. SoftRDP is
     * an LLE RDP plugin, so treating it as an RDP command buffer can make the
     * emulator wait forever on bogus DP state.
     */
}

void PJ64_CALL ProcessRDPList(void)
{
    acknowledge_dp_list();
}

void PJ64_CALL DrawScreen(void)
{
    UpdateScreen();
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
    sr_framebuffer fb;
    memset(&fb, 0, sizeof(fb));

    if (g_context) {
        (void)sr_update_screen(g_context, &fb);
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
    UpdateScreen();
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
