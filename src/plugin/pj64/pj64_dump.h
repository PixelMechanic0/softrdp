#ifndef PJ64_DUMP_H
#define PJ64_DUMP_H

#include <stdbool.h>
#include <stdint.h>

#include "pj64_gfx.h"

typedef struct sr_context sr_context;

#ifndef SOFTRDP_ENABLE_DUMP
#define SOFTRDP_ENABLE_DUMP 1
#endif

#if SOFTRDP_ENABLE_DUMP

/*
 * Set once a recording has been requested or is running. ProcessRDPList reads
 * it on every list, so it is the entire cost the non-recording path pays; the
 * locking and register reads live behind it.
 */
extern volatile bool pj64_dump_armed;

void pj64_dump_attach(const GFX_INFO *gfx, sr_context *ctx, uint32_t rdram_size);
void pj64_dump_detach(void);

/* Once per presented frame. Arms a recording when the hotkey goes down. */
void pj64_dump_poll_hotkey(void);

/* Guard each of these with pj64_dump_armed. */
void pj64_dump_before_list(void);
void pj64_dump_after_list(void);
void pj64_dump_on_present(void);

#else

#define pj64_dump_armed false

static inline void pj64_dump_attach(const GFX_INFO *gfx, sr_context *ctx, uint32_t rdram_size)
{
    (void)gfx;
    (void)ctx;
    (void)rdram_size;
}
static inline void pj64_dump_detach(void) {}
static inline void pj64_dump_poll_hotkey(void) {}
static inline void pj64_dump_before_list(void) {}
static inline void pj64_dump_after_list(void) {}
static inline void pj64_dump_on_present(void) {}

#endif

#endif
