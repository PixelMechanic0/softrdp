#include "vi.h"

#include "pipeline.h"
#include "rdp_memory.h"

#include <string.h>

#define VI_TYPE_MASK 3u
#define VI_TYPE_RGBA5551 2u
#define VI_TYPE_RGBA8888 3u

void vi_init(vi_state *vi)
{
    memset(vi, 0, sizeof(*vi));
}

void vi_latch_registers(vi_state *vi, const sr_host_interface *host)
{
    if (!vi || !host) {
        return;
    }

    vi->control = host->vi_regs[SR_VI_STATUS] ? *host->vi_regs[SR_VI_STATUS] : 0;
    vi->origin = host->vi_regs[SR_VI_ORIGIN] ? *host->vi_regs[SR_VI_ORIGIN] : 0;
    vi->width = host->vi_regs[SR_VI_WIDTH] ? *host->vi_regs[SR_VI_WIDTH] : 0;
    vi->x_scale = host->vi_regs[SR_VI_X_SCALE] ? *host->vi_regs[SR_VI_X_SCALE] : 0;
    vi->y_scale = host->vi_regs[SR_VI_Y_SCALE] ? *host->vi_regs[SR_VI_Y_SCALE] : 0;
}

static sr_rgba8 rgba8888_to_rgba8(uint32_t value)
{
    sr_rgba8 color;

    color.r = (uint8_t)(value >> 24);
    color.g = (uint8_t)(value >> 16);
    color.b = (uint8_t)(value >> 8);
    color.a = (uint8_t)value;
    return color;
}

sr_result vi_scanout(const vi_state *vi, const sr_memory *memory, sr_framebuffer *out)
{
    const uint32_t type = vi ? (vi->control & VI_TYPE_MASK) : 0;
    const uint32_t vi_width = vi ? vi->width : 0;
    const uint32_t requested_height = out ? out->height : 0;
    const uint32_t width = out && out->width ? out->width : vi_width;
    const uint32_t stride = out && out->stride_pixels ? out->stride_pixels : width;
    const uint32_t origin = vi ? (vi->origin & 0x00ffffffu) : 0;

    if (!vi || !memory || !out) {
        return SR_ERROR_INVALID_ARGUMENT;
    }

    out->width = width;
    out->stride_pixels = stride;
    out->valid = false;

    if (type != VI_TYPE_RGBA5551 && type != VI_TYPE_RGBA8888) {
        out->height = 0;
        return SR_OK;
    }

    if (vi_width == 0 || width == 0 || width > vi_width ||
        requested_height == 0 || !out->pixels || stride < width) {
        out->height = 0;
        return SR_OK;
    }

    for (uint32_t y = 0; y < requested_height; y++) {
        for (uint32_t x = 0; x < width; x++) {
            const uint32_t pixel = y * vi_width + x;
            sr_rgba8 color;

            if (type == VI_TYPE_RGBA5551) {
                uint16_t raw;
                if (!sr_memory_read_be16(memory, origin + pixel * 2u, &raw)) {
                    out->height = y;
                    return SR_ERROR_INVALID_ARGUMENT;
                }
                rdp_color rdp = pipeline_rgba5551_to_color(raw);
                color = (sr_rgba8){rdp.r, rdp.g, rdp.b, rdp.a};
            } else {
                uint32_t raw;
                if (!sr_memory_read_be32(memory, origin + pixel * 4u, &raw)) {
                    out->height = y;
                    return SR_ERROR_INVALID_ARGUMENT;
                }
                color = rgba8888_to_rgba8(raw);
            }

            out->pixels[y * stride + x] = color;
        }
    }

    out->height = requested_height;
    out->valid = true;
    return SR_OK;
}
