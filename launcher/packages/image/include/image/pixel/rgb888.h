/* rgb888: interpolation and hue generation for 0xRRGGBB colours. */
#pragma once

#include <stdint.h>

/* 0xRRGGBB `t` of `n` of the way from `lo` to `hi`, per channel, truncated.
 * A constant expression, so colour tables built from it stay in .rodata. */
#define RGB888_LERP_CH(lo, hi, shift, t, n)                                                                            \
    ((((((lo) >> (shift)) & 0xFF) * ((n) - (t)) + (((hi) >> (shift)) & 0xFF) * (t)) / (n)) & 0xFF)

#define RGB888_LERP(lo, hi, t, n)                                                                                      \
    ((RGB888_LERP_CH(lo, hi, 16, t, n) << 16) | (RGB888_LERP_CH(lo, hi, 8, t, n) << 8)                                 \
     | RGB888_LERP_CH(lo, hi, 0, t, n))

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
