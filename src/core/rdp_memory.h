#ifndef RDP_MEMORY_H
#define RDP_MEMORY_H

#include "sr_host.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

typedef struct sr_memory {
    uint8_t *rdram;
    uint32_t rdram_size;
    bool rdram_bswapped;
    uint8_t *dmem;
} sr_memory;

void sr_memory_init(sr_memory *mem, const sr_host_interface *host);

static inline uint32_t rdram_byte_index(const sr_memory *mem, uint32_t addr)
{
    return mem->rdram_bswapped ? (addr ^ 3u) : addr;
}

static inline bool sr_memory_read_u8(const sr_memory *mem, uint32_t addr, uint8_t *value)
{
    if (!mem || !mem->rdram || !value || addr >= mem->rdram_size) {
        static int printed = 0;
        if (printed < 5) {
            printf("  DEBUG: sr_memory_read_u8 failed! addr=0x%08x, rdram_size=0x%08x\n", addr, mem ? mem->rdram_size : 0);
            printed++;
        }
        return false;
    }
    *value = mem->rdram[rdram_byte_index(mem, addr)];
    return true;
}

static inline bool sr_memory_read_be16(const sr_memory *mem, uint32_t addr, uint16_t *value)
{
    if (!mem || !mem->rdram || !value || addr + 1u >= mem->rdram_size) {
        static int printed = 0;
        if (printed < 5) {
            printf("  DEBUG: sr_memory_read_be16 failed! addr=0x%08x, rdram_size=0x%08x\n", addr, mem ? mem->rdram_size : 0);
            printed++;
        }
        return false;
    }
    *value = ((uint16_t)mem->rdram[rdram_byte_index(mem, addr)] << 8) |
             ((uint16_t)mem->rdram[rdram_byte_index(mem, addr + 1u)]);
    return true;
}

static inline bool sr_memory_read_be32(const sr_memory *mem, uint32_t addr, uint32_t *value)
{
    if (!mem || !mem->rdram || !value || addr + 3u >= mem->rdram_size) {
        static int printed = 0;
        if (printed < 5) {
            printf("  DEBUG: sr_memory_read_be32 failed! addr=0x%08x, rdram_size=0x%08x\n", addr, mem ? mem->rdram_size : 0);
            printed++;
        }
        return false;
    }
    *value = ((uint32_t)mem->rdram[rdram_byte_index(mem, addr)] << 24) |
             ((uint32_t)mem->rdram[rdram_byte_index(mem, addr + 1u)] << 16) |
             ((uint32_t)mem->rdram[rdram_byte_index(mem, addr + 2u)] << 8) |
             ((uint32_t)mem->rdram[rdram_byte_index(mem, addr + 3u)]);
    return true;
}

static inline bool sr_memory_write_u8(sr_memory *mem, uint32_t addr, uint8_t value)
{
    if (!mem || !mem->rdram || addr >= mem->rdram_size) return false;
    mem->rdram[rdram_byte_index(mem, addr)] = value;
    return true;
}

static inline bool sr_memory_write_be16(sr_memory *mem, uint32_t addr, uint16_t value)
{
    if (!mem || !mem->rdram || addr + 1u >= mem->rdram_size) return false;
    mem->rdram[rdram_byte_index(mem, addr)] = (uint8_t)(value >> 8);
    mem->rdram[rdram_byte_index(mem, addr + 1u)] = (uint8_t)value;
    return true;
}

static inline bool sr_memory_write_be32(sr_memory *mem, uint32_t addr, uint32_t value)
{
    if (!mem || !mem->rdram || addr + 3u >= mem->rdram_size) return false;
    mem->rdram[rdram_byte_index(mem, addr)] = (uint8_t)(value >> 24);
    mem->rdram[rdram_byte_index(mem, addr + 1u)] = (uint8_t)(value >> 16);
    mem->rdram[rdram_byte_index(mem, addr + 2u)] = (uint8_t)(value >> 8);
    mem->rdram[rdram_byte_index(mem, addr + 3u)] = (uint8_t)value;
    return true;
}

#endif
