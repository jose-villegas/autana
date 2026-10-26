/* rgb565: native and high-byte-first RGB565 pixel arithmetic. */
#pragma once

#include <stdint.h>

typedef uint16_t rgb565be_t;

/* 0xRRGGBB to RGB565. */
#define RGB565(rgb)                                                                                                    \
    ((((uint32_t)(rgb) >> 8) & 0xF800u) | (((uint32_t)(rgb) >> 5) & 0x07E0u) | (((uint32_t)(rgb) >> 3) & 0x001Fu))

/* 0xRRGGBB to the panel's format: RGB565 with the bytes swapped, which is what
 * this QSPI controller expects, the opposite order to the chip's native
 * layout. Usable in a constant expression. */
#define RGB565BE(rgb) ((rgb565be_t)((RGB565(rgb) >> 8) | (RGB565(rgb) << 8)))

/* 0xRRGGBB `t` of `n` of the way from `lo` to `hi`, per channel, truncated.
 * A constant expression, so colour tables built from it stay in .rodata. */
#define RGB888_LERP_CH(lo, hi, shift, t, n)                                                                            \
    ((((((lo) >> (shift)) & 0xFF) * ((n) - (t)) + (((hi) >> (shift)) & 0xFF) * (t)) / (n)) & 0xFF)

#define RGB888_LERP(lo, hi, t, n)                                                                                      \
    ((RGB888_LERP_CH(lo, hi, 16, t, n) << 16) | (RGB888_LERP_CH(lo, hi, 8, t, n) << 8)                                 \
     | RGB888_LERP_CH(lo, hi, 0, t, n))

/* Panel order to native RGB565 and back: the same byte swap both ways. */
static inline uint16_t
rgb565_swap(uint16_t c) {
    return (uint16_t)((c >> 8) | (c << 8));
}

/* The channels of a NATIVE RGB565 value (rgb565_swap() a panel colour
 * first): red and blue are 5 bits, green 6. */
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

/* The panel colour for three already-quantised channels (5, 6, 5 bits),
 * what RGB565BE does after its own 8-bit-to-565 truncation, for a caller
 * (a dithered tracer, say) that quantised its own channels and only needs
 * the packing and the panel's byte swap. */
static inline rgb565be_t
rgb565be_pack(uint8_t r5, uint8_t g6, uint8_t b5) {
    return rgb565_swap((uint16_t)(((uint16_t)r5 << 11) | ((uint16_t)g6 << 5) | b5));
}

/* Channel separation leaves five multiplier bits without cross-channel carries. */
#define RGB565_SCALE_UNIT 32

/* Fast truncating fade-to-black form of rgb565be_mix: keep/UNIT of each channel. */
static inline rgb565be_t
rgb565be_scale(rgb565be_t pixel, unsigned keep) {
    const uint32_t channels = 0x07e0f81fU;
    const uint32_t native = rgb565_swap(pixel);
    const uint32_t spread = (native | (native << 16)) & channels;
    const uint32_t scaled = (spread * keep / RGB565_SCALE_UNIT) & channels;
    return rgb565_swap((uint16_t)(scaled | (scaled >> 16)));
}

/* Both halves use 32-bit multiplies on the 32-bit CPU. */
static inline uint32_t
rgb565be_pair_scale(uint32_t pair, unsigned keep) {
    return rgb565be_scale((uint16_t)pair, keep) | ((uint32_t)rgb565be_scale((uint16_t)(pair >> 16), keep) << 16);
}

/* A whole turn: six sectors of 256, so the sector is a shift and the ramp
 * within one is a byte. */
#define RGB888_HUE_TURN 1536

/* 0xRRGGBB at full saturation and full brightness. */
static inline uint32_t
rgb888_hue(int hue) {
    hue %= RGB888_HUE_TURN;
    if (hue < 0) {
        hue += RGB888_HUE_TURN;
    }

    const uint32_t ramp = (uint32_t)(hue & 0xFF); /* rising edge, 0..255 */
    const uint32_t fall = 255u - ramp;

    switch (hue >> 8) {
        case 0: return (0xFFu << 16) | (ramp << 8); /* red     -> yellow  */
        case 1: return (fall << 16) | (0xFFu << 8); /* yellow  -> green   */
        case 2: return (0xFFu << 8) | ramp;         /* green   -> cyan    */
        case 3: return (fall << 8) | 0xFFu;         /* cyan    -> blue    */
        case 4: return (ramp << 16) | 0xFFu;        /* blue    -> magenta */
        default: return (0xFFu << 16) | fall;       /* magenta -> red     */
    }
}

/* Blend `a` toward `b`. t is 0..255, 0 all `a`, 255 all `b`. A rgb565be_t
 * is RGB565 with bytes swapped (see RGB565BE above), not RGB565 itself, so
 * blending means swapping to native RGB565, unpacking R5/G6/B5, blending,
 * repacking, swapping again. Skipping either swap does not fail loudly:
 * a plausible but wrong colour, why this is tested against RGB565BE(...)
 * constants. Channels are NOT the same width (red/blue 5 bits, green 6);
 * treating all three as 8-bit shifts the hue instead of crashing. */

/* v / 255, without a divide. EXACT, not approximate, for every v these
 * callers can produce: the largest numerator rgb565be_mix() builds is
 * 63*255 + 63*255 + 127 = 32257, and this identity was checked against
 * integer division across 0..32257. Not claimed to hold outside that range,
 * which is why it is static and lives next to its one caller instead of in
 * mathi.h. Not / 255: rgb565be_mix() runs per pixel and calls this three
 * times; as hardware divides those were its dominant cost. */
static inline uint32_t
rgb565_div255(uint32_t v) {
    return (v + (v >> 8) + 1) >> 8;
}

/* The 0xRRGGBB a packed panel colour came from: the inverse of RGB565BE().
 * Expands each channel back to 8 bits by BIT REPLICATION (v<<3|v>>2 for
 * 5-bit, v<<2|v>>4 for 6-bit), not a plain shift: a plain shift leaves the
 * low bits zero, so RGB565's truncating >>3 recovers v only by
 * accident, most values landing one bucket short and reading darker each
 * round trip. Replication fills those low bits with the channel's own
 * high bits instead, so RGB565BE(rgb565be_to_rgb888(c)) == c for every c. */
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

/* Add `b` to `a`, saturating each channel at its own maximum: two strokes
 * crossing on a black field make a brighter colour rather than whichever
 * was drawn second; see boot_anim.c, where hundreds of curve segments
 * cross and flat writes made the picture look like stacked wires. Same
 * trap as rgb565be_mix() above: channels are NOT the same width.
 * Clamping all three at 31 dims green by half; at 63, red and blue wrap
 * to nearly black at their brightest, looking like holes. */
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
