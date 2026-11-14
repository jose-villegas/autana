#include "draw/draw_blend.h"

#include <stdint.h>

#include "draw/draw_blend_pie.h"
#include "simd/simd.h"

void
draw_blend_row_scalar(gfx_color_t* dst, const rgb565_mix_toward_t* m, size_t count) {
    for (size_t i = 0; i < count; i++) {
        dst[i] = rgb565be_mix_onto(dst[i], m);
    }
}

#if !SIMD_PIE
void
draw_blend_row(gfx_color_t* dst, const rgb565_mix_toward_t* m, size_t count) {
    draw_blend_row_scalar(dst, m, count);
}
#else

_Static_assert(BLEND_LANE_BYTES == sizeof(uint16_t), "a lane is one halfword");

/* `steps` of DRAW_BLEND_STEP pixels at a SIMD_ALIGN-aligned dst. */
void draw_blend_pie(gfx_color_t* dst, const uint16_t* lanes, size_t steps);

void
draw_blend_row(gfx_color_t* dst, const rgb565_mix_toward_t* m, size_t count) {
    if (count < DRAW_BLEND_MIN) {
        draw_blend_row_scalar(dst, m, count);
        return;
    }
    const size_t head = simd_head_bytes(dst) / sizeof *dst;
    draw_blend_row_scalar(dst, m, head);
    dst += head;
    count -= head;
    const uint16_t lanes[BLEND_LANES] = {
        [BLEND_LANE_RED] = BLEND_RED_MASK,
        [BLEND_LANE_GREEN] = BLEND_GREEN_MASK,
        [BLEND_LANE_BLUE] = BLEND_BLUE_MASK,
        [BLEND_LANE_RED_ADD] = (uint16_t)((m->rb >> RGB565_RB_RED_AT) + BLEND_DIV_ROUND),
        [BLEND_LANE_GREEN_ADD] = (uint16_t)(m->g + BLEND_DIV_ROUND),
        [BLEND_LANE_BLUE_ADD] = (uint16_t)((m->rb & UINT16_MAX) + BLEND_DIV_ROUND),
        [BLEND_LANE_KEEP] = (uint16_t)m->keep,
        [BLEND_LANE_DIV] = BLEND_DIV_SCALE,
    };
    const size_t steps = count / DRAW_BLEND_STEP;
    draw_blend_pie(dst, lanes, steps);
    dst += steps * DRAW_BLEND_STEP;
    count -= steps * DRAW_BLEND_STEP;
    draw_blend_row_scalar(dst, m, count);
}
#endif
