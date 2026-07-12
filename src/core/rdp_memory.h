#ifndef RDP_MEMORY_H
#define RDP_MEMORY_H

#include "sr_host.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#ifdef _MSC_VER
#include <stdlib.h>
#define bswap16 _byteswap_ushort
#define bswap32 _byteswap_ulong
#else
#define bswap16 __builtin_bswap16
#define bswap32 __builtin_bswap32
#endif

typedef struct sr_memory {
    uint8_t *rdram;
    uint32_t rdram_size;
    bool rdram_bswapped;
    uint8_t *dmem;
} sr_memory;

void sr_memory_init(sr_memory *mem, const sr_host_interface *host);

static inline uint32_t mask_addr(const sr_memory *mem, uint32_t addr)
{
    return addr & (mem->rdram_size - 1u);
}

static inline uint32_t rdram_byte_index(const sr_memory *mem, uint32_t addr)
{
    return mem->rdram_bswapped ? (addr ^ 3u) : addr;
}

static inline bool sr_memory_read_u8(const sr_memory *mem, uint32_t addr, uint8_t *value)
{
    if (!mem || !mem->rdram || !value) return false;
    addr = mask_addr(mem, addr);
    *value = mem->rdram[rdram_byte_index(mem, addr)];
    return true;
}

static inline bool sr_memory_read_be16(const sr_memory *mem, uint32_t addr, uint16_t *value)
{
    if (!mem || !mem->rdram || !value) return false;
    addr = mask_addr(mem, addr);
    if (mem->rdram_bswapped) {
        *value = (uint16_t)(*(const uint32_t *)&mem->rdram[addr & ~3u] >> (((addr & 2u) ^ 2u) * 8u));
    } else {
        *value = bswap16(*(const uint16_t *)(const void *)&mem->rdram[addr]);
    }
    return true;
}

static inline bool sr_memory_read_be32(const sr_memory *mem, uint32_t addr, uint32_t *value)
{
    if (!mem || !mem->rdram || !value) return false;
    addr = mask_addr(mem, addr);
    if (mem->rdram_bswapped) {
        *value = *(const uint32_t *)&mem->rdram[addr];
    } else {
        *value = bswap32(*(const uint32_t *)&mem->rdram[addr]);
    }
    return true;
}

static inline bool sr_memory_write_u8(sr_memory *mem, uint32_t addr, uint8_t value)
{
    if (!mem || !mem->rdram) return false;
    addr = mask_addr(mem, addr);
    mem->rdram[rdram_byte_index(mem, addr)] = value;
    return true;
}

static inline bool sr_memory_write_be16(sr_memory *mem, uint32_t addr, uint16_t value)
{
    if (!mem || !mem->rdram) return false;
    addr = mask_addr(mem, addr);
    if (mem->rdram_bswapped) {
        *(uint16_t *)&mem->rdram[addr ^ 2u] = value;
    } else {
        *(uint16_t *)&mem->rdram[addr] = bswap16(value);
    }
    return true;
}

static inline bool sr_memory_write_be32(sr_memory *mem, uint32_t addr, uint32_t value)
{
    if (!mem || !mem->rdram) return false;
    addr = mask_addr(mem, addr);
    if (mem->rdram_bswapped) {
        *(uint32_t *)&mem->rdram[addr] = value;
    } else {
        *(uint32_t *)&mem->rdram[addr] = bswap32(value);
    }
    return true;
}

#endif
