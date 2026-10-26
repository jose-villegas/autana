/* asset_bytes: little-endian fields for asset readers across the engine, read by collections/bytes.h. */
#pragma once

#include <stdint.h>

#include "collections/bytes.h"

static inline uint16_t
asset_read_u16(const uint8_t* at) {
    return bytes_read_u16(at);
}

static inline uint32_t
asset_read_u32(const uint8_t* at) {
    return bytes_read_u32(at);
}
