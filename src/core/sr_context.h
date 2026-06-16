#ifndef SR_CONTEXT_H
#define SR_CONTEXT_H

#include "rdp_memory.h"
#include "rdp_state.h"
#include "tmem.h"
#include "vi.h"

struct sr_context {
    sr_host_interface host;
    sr_memory memory;
    rdp_state rdp;
    tmem_state tmem;
    vi_state vi;
    sr_debug_stats debug;
};

#endif
