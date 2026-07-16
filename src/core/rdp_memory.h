#ifndef RDP_MEMORY_H
#define RDP_MEMORY_H

#include "sr_host.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* RDRAM uses the emulator plugin ABI's word-swapped layout: 32-bit words are
 * native-endian, while byte and halfword addresses are XOR-adjusted. */
typedef struct sr_memory {
    uint8_t *rdram;
    uint32_t rdram_size;
    uint8_t *dmem;
} sr_memory;

void sr_memory_init(sr_memory *mem, const sr_host_interface *host);

static inline uint32_t mask_addr(const sr_memory *mem, uint32_t addr)
{
    return addr & (mem->rdram_size - 1u);
}

static inline bool sr_memory_read_u8(const sr_memory *mem, uint32_t addr, uint8_t *value)
{
    if (!mem || !mem->rdram || !value) return false;
    addr = mask_addr(mem, addr);
    *value = mem->rdram[addr ^ 3u];
    return true;
}

static inline bool sr_memory_read_be16(const sr_memory *mem, uint32_t addr, uint16_t *value)
{
    if (!mem || !mem->rdram || !value) return false;
    addr = mask_addr(mem, addr);
    *value = *(const uint16_t *)(const void *)&mem->rdram[addr ^ 2u];
    return true;
}

static inline bool sr_memory_read_be32(const sr_memory *mem, uint32_t addr, uint32_t *value)
{
    if (!mem || !mem->rdram || !value) return false;
    addr = mask_addr(mem, addr);
    *value = *(const uint32_t *)(const void *)&mem->rdram[addr];
    return true;
}

static inline bool sr_memory_write_u8(sr_memory *mem, uint32_t addr, uint8_t value)
{
    if (!mem || !mem->rdram) return false;
    addr = mask_addr(mem, addr);
    mem->rdram[addr ^ 3u] = value;
    return true;
}

static inline bool sr_memory_write_be16(sr_memory *mem, uint32_t addr, uint16_t value)
{
    if (!mem || !mem->rdram) return false;
    addr = mask_addr(mem, addr);
    *(uint16_t *)(void *)&mem->rdram[addr ^ 2u] = value;
    return true;
}

static inline bool sr_memory_write_be32(sr_memory *mem, uint32_t addr, uint32_t value)
{
    if (!mem || !mem->rdram) return false;
    addr = mask_addr(mem, addr);
    *(uint32_t *)(void *)&mem->rdram[addr] = value;
    return true;
}

/* For addresses already wrapped by mask_addr() during packet setup. */
static inline uint8_t sr_memory_read_u8_fast(const sr_memory *mem, uint32_t addr)
{
    return mem->rdram[addr ^ 3u];
}

static inline uint16_t sr_memory_read_be16_fast(const sr_memory *mem, uint32_t addr)
{
    return *(const uint16_t *)(const void *)&mem->rdram[addr ^ 2u];
}

static inline uint32_t sr_memory_read_be32_fast(const sr_memory *mem, uint32_t addr)
{
    return *(const uint32_t *)(const void *)&mem->rdram[addr];
}

static inline void sr_memory_write_u8_fast(sr_memory *mem, uint32_t addr, uint8_t value)
{
    mem->rdram[addr ^ 3u] = value;
}

static inline void sr_memory_write_be16_fast(sr_memory *mem, uint32_t addr, uint16_t value)
{
    *(uint16_t *)(void *)&mem->rdram[addr ^ 2u] = value;
}

static inline void sr_memory_write_be32_fast(sr_memory *mem, uint32_t addr, uint32_t value)
{
    *(uint32_t *)(void *)&mem->rdram[addr] = value;
}

#endif
