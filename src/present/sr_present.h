#ifndef SR_PRESENT_H
#define SR_PRESENT_H

#include "../core/sr_defs.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef _WIN32
#include <windows.h>
#else
typedef void *HWND;
#endif

typedef struct sr_present {
    HWND hwnd;
    void *hdc;
    void *glrc;
    uint32_t texture;
    uint32_t program;
    uint32_t vao;
    uint32_t frame_width;
    uint32_t frame_height;
    bool ready;
    bool has_frame;
} sr_present;

bool sr_present_init(sr_present *present, HWND hwnd);
void sr_present_shutdown(sr_present *present);
void sr_present_clear(sr_present *present);
void sr_present_draw(sr_present *present);
bool sr_present_upload_rgba8(sr_present *present,
                             const sr_rgba8 *pixels,
                             uint32_t width,
                             uint32_t height,
                             uint32_t stride_pixels);

#endif
