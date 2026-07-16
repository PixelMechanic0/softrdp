#include "rdp_memory.h"

void sr_memory_init(sr_memory *mem, const sr_host_interface *host)
{
    mem->rdram = host ? host->rdram : NULL;
    mem->rdram_size = host ? host->rdram_size : 0;
    mem->dmem = host ? host->dmem : NULL;
}
