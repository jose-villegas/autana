/* bytes: little-endian fields read from stored bytes at any alignment, the order every pack and wire format here
 * uses. */
#pragma once

#include <stdint.h>

/* The little-endian u16 at `at`. */
static inline uint16_t
bytes_read_u16(const uint8_t* at) {
    return (uint16_t)((uint16_t)at[0] | ((uint16_t)at[1] << 8));
}

/* The little-endian u32 at `at`. */
static inline uint32_t
bytes_read_u32(const uint8_t* at) {
    return (uint32_t)at[0] | ((uint32_t)at[1] << 8) | ((uint32_t)at[2] << 16) | ((uint32_t)at[3] << 24);
}
