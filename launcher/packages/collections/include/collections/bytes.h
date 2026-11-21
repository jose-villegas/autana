/* bytes: little-endian fields read or written in stored bytes at any alignment, the order every pack and wire format here
 * uses. */
#pragma once

#include <limits.h>
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

static inline void
bytes_write_u16(uint8_t* at, uint16_t value) {
    at[0] = (uint8_t)value;
    at[1] = (uint8_t)(value >> CHAR_BIT);
}

static inline void
bytes_write_u32(uint8_t* at, uint32_t value) {
    bytes_write_u16(at, (uint16_t)value);
    bytes_write_u16(at + sizeof(uint16_t), (uint16_t)(value >> (sizeof(uint16_t) * CHAR_BIT)));
}

static inline void
bytes_write_u64(uint8_t* at, uint64_t value) {
    bytes_write_u32(at, (uint32_t)value);
    bytes_write_u32(at + sizeof(uint32_t), (uint32_t)(value >> (sizeof(uint32_t) * CHAR_BIT)));
}
