/* rgb565: native and high-byte-first RGB565 pixel arithmetic. */
#pragma once

#include <stdint.h>

/* RGB565 with its two bytes swapped (high byte first in memory); rgb565_swap() converts to native. */
typedef uint16_t rgb565be_t;

/* 0xRRGGBB to RGB565. */
#define RGB565(rgb)                                                                                                    \
    ((((uint32_t)(rgb) >> 8) & 0xF800u) | (((uint32_t)(rgb) >> 5) & 0x07E0u) | (((uint32_t)(rgb) >> 3) & 0x001Fu))

/* Usable in a constant expression, so colour tables built from it live in flash, not RAM. */
#define RGB565BE(rgb) ((rgb565be_t)((RGB565(rgb) >> 8) | (RGB565(rgb) << 8)))

/* Native RGB565 to high byte first and back: the same byte swap both ways. */
static inline uint16_t
rgb565_swap(uint16_t c) {
    return (uint16_t)((c >> 8) | (c << 8));
}

/* Native RGB565 channels: swap a high-byte-first colour before extracting them. */
static inline unsigned
rgb565_r5(uint16_t native) {
    return (native >> 11) & 0x1Fu;
}

#define RGB565_G6_MAX 0x3Fu

static inline unsigned
rgb565_g6(uint16_t native) {
    return (native >> 5) & RGB565_G6_MAX;
}

static inline unsigned
rgb565_b5(uint16_t native) {
    return native & 0x1Fu;
}

/* Pack already-quantised channels (5, 6, 5 bits) in high-byte-first order. */
static inline rgb565be_t
rgb565be_pack(uint8_t r5, uint8_t g6, uint8_t b5) {
    return rgb565_swap((uint16_t)(((uint16_t)r5 << 11) | ((uint16_t)g6 << 5) | b5));
}

/* Channel separation leaves five multiplier bits without cross-channel carries. */
#define RGB565_SCALE_UNIT      32

/* A native colour spread over 32 bits: green in the high half, red and blue in
 * the low, each with room above it for a product by up to RGB565_SCALE_UNIT. */
#define RGB565_SPREAD_CHANNELS 0x07e0f81fU
/* The lowest bit of each spread channel. */
#define RGB565_SPREAD_ONES     0x00200801U

static inline uint32_t
rgb565be_spread(rgb565be_t pixel) {
    const uint32_t native = rgb565_swap(pixel);
    return (native | (native << 16)) & RGB565_SPREAD_CHANNELS;
}

static inline rgb565be_t
rgb565be_unspread(uint32_t spread) {
    spread &= RGB565_SPREAD_CHANNELS;
    return rgb565_swap((uint16_t)(spread | (spread >> 16)));
}

/* Fast truncating fade-to-black form of rgb565be_mix: keep/UNIT of each channel. */
static inline rgb565be_t
rgb565be_scale(rgb565be_t pixel, unsigned keep) {
    return rgb565be_unspread(rgb565be_spread(pixel) * keep / RGB565_SCALE_UNIT);
}

/* Fast rounded form of rgb565be_mix: `share`/UNIT of b, the rest of a. */
static inline rgb565be_t
rgb565be_blend(rgb565be_t a, rgb565be_t b, unsigned share) {
    const uint32_t sum = (rgb565be_spread(a) * (RGB565_SCALE_UNIT - share)) + (rgb565be_spread(b) * share)
                         + (RGB565_SPREAD_ONES * (RGB565_SCALE_UNIT / 2));
    return rgb565be_unspread(sum / RGB565_SCALE_UNIT);
}

/* Both halves use 32-bit multiplies on the 32-bit CPU. */
static inline uint32_t
rgb565be_pair_scale(uint32_t pair, unsigned keep) {
    return rgb565be_scale((uint16_t)pair, keep) | ((uint32_t)rgb565be_scale((uint16_t)(pair >> 16), keep) << 16);
}

/* Exact v / 255 for numerators up to 32257 (63*255 + 63*255 + 127).
 * Avoids three hardware divides per blended pixel. */
static inline uint32_t
rgb565_div255(uint32_t v) {
    return (v + (v >> 8) + 1) >> 8;
}

/* Expand to 0xRRGGBB by bit replication: filling the low bits preserves
 * brightness and makes RGB565BE(rgb565be_to_rgb888(c)) == c for every c. */
static inline uint32_t
rgb565be_to_rgb888(rgb565be_t c) {
    const uint16_t native = rgb565_swap(c);
    const uint8_t r5 = (uint8_t)rgb565_r5(native);
    const uint8_t g6 = (uint8_t)rgb565_g6(native);
    const uint8_t b5 = (uint8_t)rgb565_b5(native);

    const uint8_t r8 = (uint8_t)((r5 << 3) | (r5 >> 2));
    const uint8_t g8 = (uint8_t)((g6 << 2) | (g6 >> 4));
    const uint8_t b8 = (uint8_t)((b5 << 3) | (b5 >> 2));

    return ((uint32_t)r8 << 16) | ((uint32_t)g8 << 8) | b8;
}

/* Blend a toward b: t=0 keeps a, t=255 gives b. Red/blue have five bits,
 * green six; mixing in native channel widths preserves the hue. */
static inline rgb565be_t
rgb565be_mix(rgb565be_t a, rgb565be_t b, uint8_t t) {
    const uint16_t na = rgb565_swap(a);
    const uint16_t nb = rgb565_swap(b);
    const uint8_t ar = (uint8_t)rgb565_r5(na);
    const uint8_t ag = (uint8_t)rgb565_g6(na);
    const uint8_t ab = (uint8_t)rgb565_b5(na);
    const uint8_t br = (uint8_t)rgb565_r5(nb);
    const uint8_t bg = (uint8_t)rgb565_g6(nb);
    const uint8_t bb = (uint8_t)rgb565_b5(nb);

    /* (channel * (255 - t) + channel * t) / 255, rounded rather than
     * truncated so t=255 lands exactly on `b` and t=0 exactly on `a`.
     * The divide is done as rgb565_div255(), not an approximation, an exact
     * identity over the range these numerators can reach. */
    const uint8_t mr = (uint8_t)rgb565_div255(ar * (255 - t) + br * t + 127);
    const uint8_t mg = (uint8_t)rgb565_div255(ag * (255 - t) + bg * t + 127);
    const uint8_t mb = (uint8_t)rgb565_div255(ab * (255 - t) + bb * t + 127);

    return rgb565_swap((uint16_t)((mr << 11) | (mg << 5) | mb));
}

/* Saturate each channel at its own maximum: unequal channel widths mean
 * a shared limit would dim green or wrap red and blue. */
static inline rgb565be_t
rgb565be_add(rgb565be_t a, rgb565be_t b) {
    const uint16_t na = rgb565_swap(a);
    const uint16_t nb = rgb565_swap(b);
    uint16_t r = (uint16_t)(rgb565_r5(na) + rgb565_r5(nb));
    uint16_t g = (uint16_t)(rgb565_g6(na) + rgb565_g6(nb));
    uint16_t bl = (uint16_t)(rgb565_b5(na) + rgb565_b5(nb));

    if (r > 0x1Fu) {
        r = 0x1Fu;
    }
    if (g > 0x3Fu) {
        g = 0x3Fu;
    }
    if (bl > 0x1Fu) {
        bl = 0x1Fu;
    }

    return rgb565_swap((uint16_t)((r << 11) | (g << 5) | bl));
}
