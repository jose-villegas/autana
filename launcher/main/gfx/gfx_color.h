/*
 * gfx_color: what a pixel is, separately from how the panel works.
 *
 * Pure arithmetic: no BSP, drivers or hardware headers. Code which only
 * describes colours (a table of named colours, say) can therefore be compiled
 * and tested on a host.
 *
 * The macros matter for more than tidiness. A colour table built from them is
 * a compile-time constant, so it lands in .rodata and is memory-mapped from
 * flash at zero cost in RAM, which on this board is the resource that actually
 * runs out. Computing the same table at startup would cost real bytes of the
 * scarcest thing there is.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

/* A packed, panel-ready pixel. Produced by GFX_RGB or gfx_rgb(), stored in the
 * framebuffer, never inspected by callers. */
typedef uint16_t gfx_color_t;

/* 0xRRGGBB to RGB565. */
#define GFX_RGB565(rgb)                                                                                                \
    ((((uint32_t)(rgb) >> 8) & 0xF800u) | (((uint32_t)(rgb) >> 5) & 0x07E0u) | (((uint32_t)(rgb) >> 3) & 0x001Fu))

/* 0xRRGGBB to the panel's format: RGB565 with the bytes swapped, which is what
 * this QSPI controller expects, the opposite order to the chip's native
 * layout. Usable in a constant expression. */
#define GFX_RGB(rgb) ((gfx_color_t)((GFX_RGB565(rgb) >> 8) | (GFX_RGB565(rgb) << 8)))

/* 0xRRGGBB `t` of `n` of the way from `lo` to `hi`, per channel, truncated.
 * A constant expression, so colour tables built from it stay in .rodata. */
#define GFX_LERP_RGB888_CH(lo, hi, shift, t, n)                                                                        \
    ((((((lo) >> (shift)) & 0xFF) * ((n) - (t)) + (((hi) >> (shift)) & 0xFF) * (t)) / (n)) & 0xFF)

/* Interpolate three RGB888 channels as a constant expression. */                                                      \
#define GFX_LERP_RGB888(lo, hi, t, n)                                                                                  \
    ((GFX_LERP_RGB888_CH(lo, hi, 16, t, n) << 16) | (GFX_LERP_RGB888_CH(lo, hi, 8, t, n) << 8)                         \
     | GFX_LERP_RGB888_CH(lo, hi, 0, t, n))

/* Panel order to native RGB565 and back: the same byte swap both ways. */
static inline uint16_t
gfx_color_swap(uint16_t c) {
    return (uint16_t)((c >> 8) | (c << 8));
}

/* The channels of a NATIVE RGB565 value (gfx_color_swap() a panel colour
 * first): red and blue are 5 bits, green 6. */
static inline unsigned
gfx_rgb565_r5(uint16_t native) {
    return (native >> 11) & 0x1Fu;
}

/* Extract the six-bit green channel from native RGB565. */
static inline unsigned
gfx_rgb565_g6(uint16_t native) {
    return (native >> 5) & 0x3Fu;
}

/* Extract the five-bit blue channel from native RGB565. */
static inline unsigned
gfx_rgb565_b5(uint16_t native) {
    return native & 0x1Fu;
}

/* The panel colour for three already-quantised channels (5, 6, 5 bits),
 * what GFX_RGB does after its own 8-bit-to-565 truncation, for a caller
 * (a dithered tracer, say) that quantised its own channels and only needs
 * the packing and the panel's byte swap. */
static inline gfx_color_t
gfx_color_rgb565(uint8_t r5, uint8_t g6, uint8_t b5) {
    return gfx_color_swap((uint16_t)(((uint16_t)r5 << 11) | ((uint16_t)g6 << 5) | b5));
}

/* A whole turn: six sectors of 256, so the sector is a shift and the ramp
 * within one is a byte. */
#define GFX_HUE_TURN 1536

/* 0xRRGGBB at full saturation and full brightness. */
static inline uint32_t
gfx_hue_rgb(int hue) {
    hue %= GFX_HUE_TURN;
    if (hue < 0) {
        hue += GFX_HUE_TURN;
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

/* Blend `a` toward `b`. t is 0..255, 0 all `a`, 255 all `b`. A gfx_color_t
 * is RGB565 with bytes swapped (see GFX_RGB above), not RGB565 itself, so
 * blending means swapping to native RGB565, unpacking R5/G6/B5, blending,
 * repacking, swapping again. Skipping either swap does not fail loudly:
 * a plausible but wrong colour, why this is tested against GFX_RGB(...)
 * constants. Channels are NOT the same width (red/blue 5 bits, green 6);
 * treating all three as 8-bit shifts the hue instead of crashing. */

/* v / 255, without a divide. EXACT, not approximate, for every v these
 * callers can produce: the largest numerator gfx_color_mix() builds is
 * 63*255 + 63*255 + 127 = 32257, and this identity was checked against
 * integer division across 0..32257. Not claimed to hold outside that range,
 * which is why it is static and lives next to its one caller instead of in
 * mathi.h. Not / 255: gfx_color_mix() runs per pixel and calls this three
 * times; as hardware divides those were its dominant cost. */
static inline uint32_t
div255(uint32_t v) {
    return (v + (v >> 8) + 1) >> 8;
}

/* The 0xRRGGBB a packed panel colour came from: the inverse of GFX_RGB().
 * Expands each channel back to 8 bits by BIT REPLICATION (v<<3|v>>2 for
 * 5-bit, v<<2|v>>4 for 6-bit), not a plain shift: a plain shift leaves the
 * low bits zero, so GFX_RGB565's truncating >>3 recovers v only by
 * accident, most values landing one bucket short and reading darker each
 * round trip. Replication fills those low bits with the channel's own
 * high bits instead, so GFX_RGB(gfx_color_rgb888(c)) == c for every c. */
static inline uint32_t
gfx_color_rgb888(gfx_color_t c) {
    const uint16_t native = gfx_color_swap(c);
    const uint8_t r5 = (uint8_t)gfx_rgb565_r5(native);
    const uint8_t g6 = (uint8_t)gfx_rgb565_g6(native);
    const uint8_t b5 = (uint8_t)gfx_rgb565_b5(native);

    const uint8_t r8 = (uint8_t)((r5 << 3) | (r5 >> 2));
    const uint8_t g8 = (uint8_t)((g6 << 2) | (g6 >> 4));
    const uint8_t b8 = (uint8_t)((b5 << 3) | (b5 >> 2));

    return ((uint32_t)r8 << 16) | ((uint32_t)g8 << 8) | b8;
}

/* Blend packed panel colours with rounded RGB565 channels and t in 0..255. */
static inline gfx_color_t
gfx_color_mix(gfx_color_t a, gfx_color_t b, uint8_t t) {
    const uint16_t na = gfx_color_swap(a);
    const uint16_t nb = gfx_color_swap(b);
    const uint8_t ar = (uint8_t)gfx_rgb565_r5(na);
    const uint8_t ag = (uint8_t)gfx_rgb565_g6(na);
    const uint8_t ab = (uint8_t)gfx_rgb565_b5(na);
    const uint8_t br = (uint8_t)gfx_rgb565_r5(nb);
    const uint8_t bg = (uint8_t)gfx_rgb565_g6(nb);
    const uint8_t bb = (uint8_t)gfx_rgb565_b5(nb);

    /* (channel * (255 - t) + channel * t) / 255, rounded rather than
     * truncated so t=255 lands exactly on `b` and t=0 exactly on `a`.
     * The divide is done as div255(), not an approximation, an exact
     * identity over the range these numerators can reach. */
    const uint8_t mr = (uint8_t)div255(ar * (255 - t) + br * t + 127);
    const uint8_t mg = (uint8_t)div255(ag * (255 - t) + bg * t + 127);
    const uint8_t mb = (uint8_t)div255(ab * (255 - t) + bb * t + 127);

    return gfx_color_swap((uint16_t)((mr << 11) | (mg << 5) | mb));
}

/* The standard order-4 Bayer matrix, values 0..15 rather than pre-scaled
 * to 0..255; gfx_dither_covers() scales the ONE side that needs to be a
 * byte (alpha), not the table it is compared against. Indexed by each
 * pixel's own ABSOLUTE panel row/col (y & 3, x & 3), not a position
 * local to whatever shape is dithered; that keeps two dithered shapes
 * that overlap or sit edge to edge in phase: a local index would have
 * every shape restart the pattern at its own corner, reading as a seam
 * where two meet. */
static const uint8_t gfx_dither4x4[4][4] = {
    {0, 8, 2, 10},
    {12, 4, 14, 6},
    {3, 11, 1, 9},
    {15, 7, 13, 5},
};

/* The alpha -> Bayer-level scaling gfx_dither_covers() compares against
 * the table, exposed on its own so a caller with a whole ROW of pixels
 * at one alpha can compute the level once instead of per pixel; see
 * boot_anim.c's draw_image(). Rounded, not truncated: a plain
 * `alpha >> 4` maps every alpha in 1..15 to level 0, covering nothing;
 * +8 rounds to the nearest of 16 levels instead of flooring. 17, not 16:
 * alpha 255 maps above every table cell (max 15), keeping "fully solid"
 * exactly solid. */
static inline int
gfx_dither_level(uint8_t alpha) {
    if (alpha == 0) {
        return 0;
    }
    if (alpha >= 255) {
        return 16;
    }
    return (alpha + 8) >> 4;
}

/* Whether an ordered (Bayer) dither at `alpha` (0 nothing, 255 everything,
 * 16 graduated levels between; see gfx_fill_rect_dither()'s own comment
 * in gfx.c) covers absolute panel pixel (x, y). Callers with their own pixel
 * loops share this table and rounding rule rather than drifting out of phase. */
static inline bool
gfx_dither_covers(int x, int y, uint8_t alpha) {
    return gfx_dither_level(alpha) > gfx_dither4x4[y & 3][x & 3];
}

/* Add `b` to `a`, saturating each channel at its own maximum: two strokes
 * crossing on a black field make a brighter colour rather than whichever
 * was drawn second; see boot_anim.c, where hundreds of curve segments
 * cross and flat writes made the picture look like stacked wires. Same
 * trap as gfx_color_mix() above: channels are NOT the same width.
 * Clamping all three at 31 dims green by half; at 63, red and blue wrap
 * to nearly black at their brightest, looking like holes. */
static inline gfx_color_t
gfx_color_add(gfx_color_t a, gfx_color_t b) {
    const uint16_t na = gfx_color_swap(a);
    const uint16_t nb = gfx_color_swap(b);
    uint16_t r = (uint16_t)(gfx_rgb565_r5(na) + gfx_rgb565_r5(nb));
    uint16_t g = (uint16_t)(gfx_rgb565_g6(na) + gfx_rgb565_g6(nb));
    uint16_t bl = (uint16_t)(gfx_rgb565_b5(na) + gfx_rgb565_b5(nb));

    if (r > 0x1Fu) {
        r = 0x1Fu;
    }
    if (g > 0x3Fu) {
        g = 0x3Fu;
    }
    if (bl > 0x1Fu) {
        bl = 0x1Fu;
    }

    return gfx_color_swap((uint16_t)((r << 11) | (g << 5) | bl));
}
