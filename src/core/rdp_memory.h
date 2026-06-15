#ifndef RDP_MEMORY_H
#define RDP_MEMORY_H

#include "sr_host.h"

typedef struct sr_memory {
    uint8_t *rdram;
    uint32_t rdram_size;
    uint8_t *dmem;
} sr_memory;

void sr_memory_init(sr_memory *mem, const sr_host_interface *host);
bool sr_memory_read_u8(const sr_memory *mem, uint32_t addr, uint8_t *value);
bool sr_memory_read_be16(const sr_memory *mem, uint32_t addr, uint16_t *value);
bool sr_memory_read_be32(const sr_memory *mem, uint32_t addr, uint32_t *value);
bool sr_memory_write_u8(sr_memory *mem, uint32_t addr, uint8_t value);
bool sr_memory_write_be16(sr_memory *mem, uint32_t addr, uint16_t value);
bool sr_memory_write_be32(sr_memory *mem, uint32_t addr, uint32_t value);

#endif
