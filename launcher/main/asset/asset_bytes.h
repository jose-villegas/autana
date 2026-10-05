/*
 * asset_bytes: how the asset readers (asset_pack.c, asset_directory.c) read a
 * field. Not for their callers.
 */
#pragma once

#include <stdint.h>

/* The little-endian u32 at `at`. */
static inline uint32_t
asset_read_u32(const uint8_t* at) {
    return (uint32_t)at[0] | ((uint32_t)at[1] << 8) | ((uint32_t)at[2] << 16) | ((uint32_t)at[3] << 24);
}
