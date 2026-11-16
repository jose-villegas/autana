/*
 * rgb565_layout: where each channel lies in a native RGB565 pixel.
 * Preprocessor only, with no C suffixes, so assembly includes it too.
 */
#pragma once

/* The largest five-bit (red, blue) and six-bit (green) channel value. */
#define RGB565_C5_MAX     0x1F
#define RGB565_G6_MAX     0x3F

#define RGB565_RED_AT     11
#define RGB565_GREEN_AT   5
#define RGB565_BLUE_AT    0

#define RGB565_RED_MASK   (RGB565_C5_MAX << RGB565_RED_AT)
#define RGB565_GREEN_MASK (RGB565_G6_MAX << RGB565_GREEN_AT)
#define RGB565_BLUE_MASK  (RGB565_C5_MAX << RGB565_BLUE_AT)
