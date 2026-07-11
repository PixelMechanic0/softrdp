#ifndef DEPTH_H
#define DEPTH_H

#include <stdint.h>

uint32_t rdp_depth_decompress(uint16_t compressed);
uint16_t rdp_depth_compress(uint32_t depth);
uint32_t rdp_depth_from_span(int64_t depth_fixed);
uint32_t rdp_depth_from_primitive(uint16_t primitive_depth);
uint16_t rdp_depth_normalize_delta(int32_t dzdx, int32_t dzdy);
uint8_t rdp_depth_compress_delta(uint16_t delta);
uint32_t rdp_depth_decompress_delta(uint8_t compressed);
uint16_t rdp_depth_pack(uint32_t depth, uint8_t compressed_delta);

#endif
