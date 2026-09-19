/*
 * gfx_glow - a curve drawn as light: every pixel within a radius of the
 * curve is coloured by its distance to it.
 *
 * The curve is a height per column of a VIEW frame, turned a number of
 * quarter turns into the panel, so it is a function of x and one 1D array
 * can carry a displaced, waving copy of it. Heights are Q4.
 *
 * The distance is the true distance to the curve, not the vertical one.
 * A vertical falloff scaled by the local slope is a fraction of the work and
 * was rejected: beside a cliff it lights a spike far above the plateau,
 * because it measures to the cliff's tangent line and not to where the cliff
 * ends.
 *
 * Pure and header-only like gfx_target.h, so a host suite drives the real
 * arithmetic with no panel; gfx.c's gfx_glow_curve() adds the guards, the
 * target and the dirty marking.
 */
#pragma once

#include <stdint.h>

#include "gfx/gfx_color.h"
#include "gfx/gfx_target.h"

#define GFX_GLOW_Q_SHIFT    4
#define GFX_GLOW_ONE        (1 << GFX_GLOW_Q_SHIFT)
#define GFX_GLOW_MAX_RADIUS 31
#define GFX_GLOW_RAMP_SIZE  64
#define GFX_GLOW_PHASES     16

/* Columns drawn per call. Bounds the span scratch on the stack and sizes the
 * box each call reports for dirty marking. */
#define GFX_GLOW_CHUNK      16

typedef struct {
    /* Colour by distance over radius, [0] on the curve, once per cell of
     * gfx_dither4x4. RGB565 has 32 levels of red and blue, and a glow fading
     * to black through them bands; each phase rounds the same 8-bit colour
     * at a different threshold, so the bands break up into a gradient. */
    gfx_color_t ramp[GFX_GLOW_PHASES][GFX_GLOW_RAMP_SIZE];
    /* chord[k]: how far light reaches vertically, k columns away. Q4. */
    int16_t chord[GFX_GLOW_MAX_RADIUS + 1];
    int radius;
    uint32_t u_per_d2; /* (d/r)^2 in Q12, per unit of Q8 distance squared, << 16 */
} gfx_glow_style_t;

typedef struct {
    int x0, y0, x1, y1; /* panel space, half open; empty when x1 <= x0 */
} gfx_glow_box_t;

static inline uint32_t
gfx_glow_isqrt(uint32_t v) {
    uint32_t root = 0;
    for (uint32_t bit = 1u << 30; bit != 0; bit >>= 2) {
        if (v >= root + bit) {
            v -= root + bit;
            root = (root >> 1) + bit;
        } else {
            root >>= 1;
        }
    }
    return root;
}

/* sqrt(i / 256) * 256. One table serves two ranges - see gfx_glow_ramp_index(). */
static const uint8_t gfx_glow_sqrt_q8[256] = {
    0,   16,  23,  28,  32,  36,  39,  42,  45,  48,  51,  53,  55,  58,  60,  62,  64,  66,  68,  70,  72,  73,
    75,  77,  78,  80,  82,  83,  85,  86,  88,  89,  91,  92,  93,  95,  96,  97,  99,  100, 101, 102, 104, 105,
    106, 107, 109, 110, 111, 112, 113, 114, 115, 116, 118, 119, 120, 121, 122, 123, 124, 125, 126, 127, 128, 129,
    130, 131, 132, 133, 134, 135, 136, 137, 138, 139, 139, 140, 141, 142, 143, 144, 145, 146, 147, 148, 148, 149,
    150, 151, 152, 153, 153, 154, 155, 156, 157, 158, 158, 159, 160, 161, 162, 162, 163, 164, 165, 166, 166, 167,
    168, 169, 169, 170, 171, 172, 172, 173, 174, 175, 175, 176, 177, 177, 178, 179, 180, 180, 181, 182, 182, 183,
    184, 185, 185, 186, 187, 187, 188, 189, 189, 190, 191, 191, 192, 193, 193, 194, 195, 195, 196, 197, 197, 198,
    199, 199, 200, 200, 201, 202, 202, 203, 204, 204, 205, 206, 206, 207, 207, 208, 209, 209, 210, 210, 211, 212,
    212, 213, 213, 214, 215, 215, 216, 216, 217, 218, 218, 219, 219, 220, 221, 221, 222, 222, 223, 223, 224, 225,
    225, 226, 226, 227, 227, 228, 229, 229, 230, 230, 231, 231, 232, 232, 233, 234, 234, 235, 235, 236, 236, 237,
    237, 238, 238, 239, 239, 240, 241, 241, 242, 242, 243, 243, 244, 244, 245, 245, 246, 246, 247, 247, 248, 248,
    249, 249, 250, 250, 251, 251, 252, 252, 253, 253, 254, 254, 255, 255,
};

/* Distance over radius as a ramp index, from its square in Q12, with no
 * square root: sqrt(u) = sqrt(16 u) / 4, so the lowest sixteenth of the
 * range - the core, where a step of distance is most visible - reads the
 * same table at sixteen times the resolution. */
static inline uint32_t
gfx_glow_ramp_index(uint32_t u_q12) {
    const uint32_t t_q8 = u_q12 < 256 ? gfx_glow_sqrt_q8[u_q12] >> 2 : gfx_glow_sqrt_q8[u_q12 >> 4];
    return t_q8 * GFX_GLOW_RAMP_SIZE >> 8;
}

/* An 8-bit channel to `levels` steps, rounded up once the remainder passes
 * this phase's threshold. */
static inline uint32_t
gfx_glow_quantise(uint32_t channel, uint32_t levels, uint32_t phase) {
    const uint32_t scaled_q4 = channel * levels * GFX_GLOW_PHASES / 255;
    const uint32_t level = (scaled_q4 + phase) >> GFX_GLOW_Q_SHIFT;
    return level > levels ? levels : level;
}

static inline uint32_t
gfx_glow_lerp_channel(uint32_t from, uint32_t to, uint32_t t_q8, int shift) {
    const uint32_t a = (from >> shift) & 0xFF;
    const uint32_t b = (to >> shift) & 0xFF;
    return (a * (256 - t_q8) + b * t_q8) >> 8;
}

/* Light falls off as (1 - d/r)^2, `core_rgb` on the curve fading to
 * `halo_rgb` over the first `core_px`, and to black at `radius_px`. */
static inline void
gfx_glow_style_set(gfx_glow_style_t* style, int radius_px, int core_px, uint32_t core_rgb, uint32_t halo_rgb) {
    if (radius_px < 1) {
        radius_px = 1;
    }
    if (radius_px > GFX_GLOW_MAX_RADIUS) {
        radius_px = GFX_GLOW_MAX_RADIUS;
    }
    if (core_px < 1) {
        core_px = 1;
    }
    if (core_px > radius_px) {
        core_px = radius_px;
    }
    style->radius = radius_px;

    const uint32_t r2_q8 = (uint32_t)(radius_px * GFX_GLOW_ONE) * (uint32_t)(radius_px * GFX_GLOW_ONE);
    style->u_per_d2 = ((uint32_t)4096 << 16) / r2_q8;

    for (int k = 0; k <= GFX_GLOW_MAX_RADIUS; k++) {
        const int inside = radius_px * radius_px - k * k;
        style->chord[k] = inside > 0 ? (int16_t)gfx_glow_isqrt((uint32_t)inside << (2 * GFX_GLOW_Q_SHIFT)) : 0;
    }

    const uint32_t core_t_q8 = (uint32_t)core_px * 256 / (uint32_t)radius_px;
    for (int i = 0; i < GFX_GLOW_RAMP_SIZE; i++) {
        const uint32_t t_q8 = (uint32_t)i * 256 / GFX_GLOW_RAMP_SIZE;
        const uint32_t rest = 256 - t_q8;
        const uint32_t light_q8 = (rest * rest) >> 8;
        const uint32_t halo_q8 = t_q8 < core_t_q8 ? t_q8 * 256 / core_t_q8 : 256;
        const uint32_t r = (gfx_glow_lerp_channel(core_rgb, halo_rgb, halo_q8, 16) * light_q8) >> 8;
        const uint32_t g = (gfx_glow_lerp_channel(core_rgb, halo_rgb, halo_q8, 8) * light_q8) >> 8;
        const uint32_t b = (gfx_glow_lerp_channel(core_rgb, halo_rgb, halo_q8, 0) * light_q8) >> 8;
        for (uint32_t phase = 0; phase < GFX_GLOW_PHASES; phase++) {
            const uint32_t rgb565 = (gfx_glow_quantise(r, 31, phase) << 11) | (gfx_glow_quantise(g, 63, phase) << 5)
                                    | gfx_glow_quantise(b, 31, phase);
            style->ramp[phase][i] = (gfx_color_t)((rgb565 >> 8) | (rgb565 << 8));
        }
    }
}

/* What a column covers vertically: the curve runs straight between column
 * centres, so from the midpoint with one neighbour to the midpoint with the
 * other, and through its own height where that is a peak or a dip. */
static inline void
gfx_glow_column_span(const int16_t* y, int count, int x, int* lo, int* hi) {
    const int here = y[x];
    const int left = x > 0 ? (y[x - 1] + here) / 2 : here;
    const int right = x + 1 < count ? (here + y[x + 1]) / 2 : here;
    *lo = here;
    *hi = here;
    if (left < *lo) {
        *lo = left;
    }
    if (right < *lo) {
        *lo = right;
    }
    if (left > *hi) {
        *hi = left;
    }
    if (right > *hi) {
        *hi = right;
    }
}

static inline int
gfx_glow_outside(int v, int lo, int hi) {
    return v < lo ? lo - v : (v > hi ? v - hi : 0);
}

static inline void
gfx_glow_to_panel(int quarter_turns, int panel_w, int panel_h, int x, int y, int* px, int* py) {
    switch (quarter_turns & 3) {
        case 1:
            *px = panel_w - 1 - y;
            *py = x;
            break;
        case 2:
            *px = panel_w - 1 - x;
            *py = panel_h - 1 - y;
            break;
        case 3:
            *px = y;
            *py = panel_h - 1 - x;
            break;
        default:
            *px = x;
            *py = y;
            break;
    }
}

static inline void
gfx_glow_box_add(gfx_glow_box_t* box, int px, int py) {
    if (box->x1 <= box->x0) {
        *box = (gfx_glow_box_t){px, py, px + 1, py + 1};
        return;
    }
    if (px < box->x0) {
        box->x0 = px;
    }
    if (py < box->y0) {
        box->y0 = py;
    }
    if (px >= box->x1) {
        box->x1 = px + 1;
    }
    if (py >= box->y1) {
        box->y1 = py + 1;
    }
}

/* The spans of the columns a chunk can see: its own and `radius` to each
 * side. Index 0 holds column `first`. */
typedef struct {
    int16_t lo[GFX_GLOW_CHUNK + 2 * GFX_GLOW_MAX_RADIUS];
    int16_t hi[GFX_GLOW_CHUNK + 2 * GFX_GLOW_MAX_RADIUS];
    int first;
    int count;
} gfx_glow_spans_t;

static inline void
gfx_glow_spans_fill(gfx_glow_spans_t* spans, const int16_t* y, int count, int x0, int x1, int radius) {
    spans->first = x0 - radius;
    spans->count = count;
    for (int j = spans->first; j < x1 + radius; j++) {
        if (j >= 0 && j < count) {
            int lo, hi;
            gfx_glow_column_span(y, count, j, &lo, &hi);
            spans->lo[j - spans->first] = (int16_t)lo;
            spans->hi[j - spans->first] = (int16_t)hi;
        }
    }
}

/* The rows of column `x` that light can reach: every column within the
 * radius lights a chord of it. Q4. */
static inline void
gfx_glow_reach(const gfx_glow_spans_t* spans, const gfx_glow_style_t* style, int x, int* lo, int* hi) {
    *lo = INT32_MAX;
    *hi = INT32_MIN;
    for (int k = -style->radius; k <= style->radius; k++) {
        const int j = x + k;
        if (j < 0 || j >= spans->count) {
            continue;
        }
        const int chord = style->chord[k < 0 ? -k : k];
        if (spans->lo[j - spans->first] - chord < *lo) {
            *lo = spans->lo[j - spans->first] - chord;
        }
        if (spans->hi[j - spans->first] + chord > *hi) {
            *hi = spans->hi[j - spans->first] + chord;
        }
    }
}

/* Squared distance, Q8, from (x, centre) to the curve. Walks outward a column
 * at a time and stops once a column is further across than the best found,
 * which near a flat stretch is after a handful. */
static inline int
gfx_glow_distance2(const gfx_glow_spans_t* spans, int radius, int x, int centre) {
    const int own = gfx_glow_outside(centre, spans->lo[x - spans->first], spans->hi[x - spans->first]);
    int best = own * own;
    for (int k = 1; k <= radius; k++) {
        const int across = (k * GFX_GLOW_ONE) * (k * GFX_GLOW_ONE);
        if (across >= best) {
            break;
        }
        for (int side = -1; side <= 1; side += 2) {
            const int j = x + side * k;
            if (j < 0 || j >= spans->count) {
                continue;
            }
            const int up = gfx_glow_outside(centre, spans->lo[j - spans->first], spans->hi[j - spans->first]);
            if (across + up * up < best) {
                best = across + up * up;
            }
        }
    }
    return best;
}

static inline gfx_color_t
gfx_glow_colour(const gfx_glow_style_t* style, int distance2, int px, int py) {
    const uint32_t u_q12 = ((uint32_t)distance2 * style->u_per_d2) >> 16;
    if (u_q12 >= 4096) {
        return GFX_RGB(0x000000);
    }
    return style->ramp[gfx_dither4x4[py & 3][px & 3]][gfx_glow_ramp_index(u_q12)];
}

/*
 * Draws columns [x0, x1) of the curve, at most GFX_GLOW_CHUNK of them, and
 * returns the panel box it wrote. Rows within `erase_px` beyond the light's
 * reach are written black, so a curve that moves less than that per frame
 * wipes its own trail. Every pixel written is replaced, not blended.
 */
static inline gfx_glow_box_t
gfx_glow_draw_columns(gfx_target_t target, int clip_x0, int clip_y0, int clip_x1, int clip_y1, int panel_w, int panel_h,
                      const int16_t* y, int count, int x0, int x1, int quarter_turns, int erase_px,
                      const gfx_glow_style_t* style) {
    gfx_glow_box_t box = {0, 0, 0, 0};
    const int erase = erase_px * GFX_GLOW_ONE;

    x0 = x0 < 0 ? 0 : x0;
    x1 = x1 > count ? count : x1;
    x1 = x1 - x0 > GFX_GLOW_CHUNK ? x0 + GFX_GLOW_CHUNK : x1;
    if (x1 <= x0) {
        return box;
    }

    gfx_glow_spans_t spans;
    gfx_glow_spans_fill(&spans, y, count, x0, x1, style->radius);

    for (int x = x0; x < x1; x++) {
        int reach_lo, reach_hi;
        gfx_glow_reach(&spans, style, x, &reach_lo, &reach_hi);
        const int row0 = (reach_lo - erase) >> GFX_GLOW_Q_SHIFT;
        const int row1 = ((reach_hi + erase) >> GFX_GLOW_Q_SHIFT) + 1;

        for (int row = row0; row < row1; row++) {
            int px, py;
            gfx_glow_to_panel(quarter_turns, panel_w, panel_h, x, row, &px, &py);
            if (px < clip_x0 || px >= clip_x1 || py < clip_y0 || py >= clip_y1 || py < target.y0
                || py >= target.y0 + target.height) {
                continue;
            }
            const int centre = row * GFX_GLOW_ONE + GFX_GLOW_ONE / 2;
            const int distance2 = gfx_glow_distance2(&spans, style->radius, x, centre);
            gfx_target_row(target, py)[px] = gfx_glow_colour(style, distance2, px, py);
            gfx_glow_box_add(&box, px, py);
        }
    }
    return box;
}
