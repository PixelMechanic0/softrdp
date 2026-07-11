#include "depth.h"

static unsigned highest_set_bit(uint32_t value)
{
    unsigned bit = 0;
    while (value >>= 1u) bit++;
    return bit;
}

uint32_t rdp_depth_decompress(uint16_t compressed)
{
    const uint32_t encoded = compressed >> 2;
    const uint32_t exponent = encoded >> 11;
    const uint32_t mantissa = encoded & 0x7ffu;
    const uint32_t shift = exponent < 6u ? 6u - exponent : 0u;
    const uint32_t base = 0x40000u - (0x40000u >> exponent);
    return ((mantissa << shift) + base) & 0x3ffffu;
}

uint16_t rdp_depth_compress(uint32_t depth)
{
    depth &= 0x3ffffu;
    const uint32_t inverse = 0x3ffffu - depth > 0u ? 0x3ffffu - depth : 1u;
    int exponent = 17 - (int)highest_set_bit(inverse);
    if (exponent < 0) exponent = 0;
    if (exponent > 7) exponent = 7;
    const uint32_t shift = exponent < 6 ? 6u - (uint32_t)exponent : 0u;
    const uint32_t mantissa = (depth >> shift) & 0x7ffu;
    const uint16_t encoded = (uint16_t)(((uint32_t)exponent << 11) | mantissa);
    return (uint16_t)(encoded << 2);
}

uint32_t rdp_depth_from_span(int64_t depth_fixed)
{
    return (uint32_t)((uint64_t)depth_fixed >> 10) & 0x3ffffu;
}

uint32_t rdp_depth_from_primitive(uint16_t primitive_depth)
{
    return ((uint32_t)primitive_depth << 6) & 0x3ffffu;
}

uint16_t rdp_depth_normalize_delta(int32_t dzdx, int32_t dzdy)
{
    uint32_t x = ((uint32_t)dzdx >> 16) & 0xffffu;
    uint32_t y = ((uint32_t)dzdy >> 16) & 0xffffu;
    x = (x & 0x8000u) ? ((~x) & 0x7fffu) : x;
    y = (y & 0x8000u) ? ((~y) & 0x7fffu) : y;
    const uint32_t sum = (x + y) & 0xffffu;
    if (sum & 0xc000u) return 0x8000u;
    if (sum == 0u) return 1u;
    if (sum == 1u) return 3u;
    uint32_t bit = 0x2000u;
    while (bit && !(sum & bit)) bit >>= 1u;
    return (uint16_t)(bit << 1u);
}

uint8_t rdp_depth_compress_delta(uint16_t delta)
{
    if (!delta) return 0u;
    uint8_t bit = 0u;
    while (delta >>= 1u) bit++;
    return bit;
}

uint32_t rdp_depth_decompress_delta(uint8_t compressed)
{
    return 1u << (compressed & 15u);
}

uint16_t rdp_depth_pack(uint32_t depth, uint8_t compressed_delta)
{
    return (uint16_t)(rdp_depth_compress(depth) | ((compressed_delta >> 2) & 3u));
}
