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
    if (!vi || !host) return;
    #define LATCH(reg) (host->vi_regs[reg] ? *host->vi_regs[reg] : 0)
    vi->control = LATCH(SR_VI_STATUS);
    vi->origin  = LATCH(SR_VI_ORIGIN);
    vi->width   = LATCH(SR_VI_WIDTH);
    vi->x_scale = LATCH(SR_VI_X_SCALE);
    vi->y_scale = LATCH(SR_VI_Y_SCALE);
    #undef LATCH
}

#define VI_MAX_WIDTH 1024u

static sr_rgba8 rgba8888_to_rgba8(uint32_t value)
{
    return (sr_rgba8){ (uint8_t)(value >> 24), (uint8_t)(value >> 16), (uint8_t)(value >> 8), (uint8_t)value };
}

static inline void vi_filter_dedither(sr_rgba8 *scanline, uint32_t width)
{
    (void)scanline; (void)width; // Placeholder for de-dither filter
}

static inline void vi_filter_divot(sr_rgba8 *scanline, uint32_t width)
{
    (void)scanline; (void)width; // Placeholder for divot filter
}

sr_result vi_scanout(const vi_state *vi, const sr_memory *memory, sr_framebuffer *out)
{
    if (!vi || !memory || !out) return SR_ERROR_INVALID_ARGUMENT;

    const uint32_t type = vi->control & VI_TYPE_MASK;
    const uint32_t vi_width = vi->width;
    const uint32_t requested_height = out->height;
    const uint32_t width = out->width ? out->width : vi_width;
    const uint32_t stride = out->stride_pixels ? out->stride_pixels : width;
    const uint32_t origin = vi->origin & 0x00ffffffu;

    out->width = width;
    out->stride_pixels = stride;
    out->valid = false;

    if (type != VI_TYPE_RGBA5551 && type != VI_TYPE_RGBA8888) {
        out->height = 0;
        return SR_OK;
    }

    if (vi_width == 0 || width == 0 || width > vi_width || width > VI_MAX_WIDTH || requested_height == 0 || !out->pixels || stride < width) {
        out->height = 0;
        return SR_OK;
    }

    sr_rgba8 scanline[VI_MAX_WIDTH];

    for (uint32_t y = 0; y < requested_height; y++) {
        if (type == VI_TYPE_RGBA5551) {
            for (uint32_t x = 0; x < width; x++) {
                uint16_t raw;
                const uint32_t pixel = y * vi_width + x;
                if (!sr_memory_read_be16(memory, origin + pixel * 2u, &raw)) {
                    out->height = y;
                    return SR_ERROR_INVALID_ARGUMENT;
                }
                rdp_color rdp = pipeline_rgba5551_to_color(raw);
                scanline[x] = (sr_rgba8){rdp.r, rdp.g, rdp.b, rdp.a};
            }
        } else {
            for (uint32_t x = 0; x < width; x++) {
                uint32_t raw;
                const uint32_t pixel = y * vi_width + x;
                if (!sr_memory_read_be32(memory, origin + pixel * 4u, &raw)) {
                    out->height = y;
                    return SR_ERROR_INVALID_ARGUMENT;
                }
                scanline[x] = rgba8888_to_rgba8(raw);
            }
        }

        vi_filter_dedither(scanline, width);
        vi_filter_divot(scanline, width);

        memcpy(&out->pixels[y * stride], scanline, width * sizeof(sr_rgba8));
    }

    out->height = requested_height;
    out->valid = true;
    return SR_OK;
}
