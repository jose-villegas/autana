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

#include <stdbool.h>
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

/* Light held to `steps` equal levels, the remainder decided by this phase's
 * threshold, so the falloff shows as a stipple that thins toward the rim
 * rather than as a smooth fade. No steps leaves it smooth. */
static inline uint32_t
gfx_glow_stepped_light(uint32_t light_q8, uint32_t steps, uint32_t phase) {
    if (steps == 0) {
        return light_q8;
    }
    const uint32_t scaled_q4 = (light_q8 * steps * GFX_GLOW_PHASES) >> 8;
    const uint32_t level = (scaled_q4 + phase) >> GFX_GLOW_Q_SHIFT;
    return (level > steps ? steps : level) * 256 / steps;
}

/* Light falls off as (1 - d/r)^2, `core_rgb` on the curve fading to
 * `halo_rgb` over the first `core_px`, and to black at `radius_px`, in
 * `steps` levels of stipple or smoothly for none. */
static inline void
gfx_glow_style_set_stepped(gfx_glow_style_t* style, int radius_px, int core_px, uint32_t core_rgb, uint32_t halo_rgb,
                           int steps) {
    if (steps < 0) {
        steps = 0;
    }
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
        const uint32_t halo_q8 = t_q8 < core_t_q8 ? t_q8 * 256 / core_t_q8 : 256;
        for (uint32_t phase = 0; phase < GFX_GLOW_PHASES; phase++) {
            const uint32_t light_q8 = gfx_glow_stepped_light((rest * rest) >> 8, (uint32_t)steps, phase);
            const uint32_t r = (gfx_glow_lerp_channel(core_rgb, halo_rgb, halo_q8, 16) * light_q8) >> 8;
            const uint32_t g = (gfx_glow_lerp_channel(core_rgb, halo_rgb, halo_q8, 8) * light_q8) >> 8;
            const uint32_t b = (gfx_glow_lerp_channel(core_rgb, halo_rgb, halo_q8, 0) * light_q8) >> 8;
            const uint32_t rgb565 = (gfx_glow_quantise(r, 31, phase) << 11) | (gfx_glow_quantise(g, 63, phase) << 5)
                                    | gfx_glow_quantise(b, 31, phase);
            style->ramp[phase][i] = (gfx_color_t)((rgb565 >> 8) | (rgb565 << 8));
        }
    }
}

static inline void
gfx_glow_style_set(gfx_glow_style_t* style, int radius_px, int core_px, uint32_t core_rgb, uint32_t halo_rgb) {
    gfx_glow_style_set_stepped(style, radius_px, core_px, core_rgb, halo_rgb, 0);
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

/*
 * The curve at any angle. A pose is where the view frame's DOWN points on
 * the panel, a unit vector in Q14, and both frames turn about their centres;
 * down = (-1, 0) is quarter turn 1, pixel for pixel. A turned curve is not a
 * height per panel column, so this walks panel rows and asks of each pixel
 * where it lies in the view frame.
 */

#define GFX_GLOW_POSE_ONE     (1 << 14)

/* A posed row is walked in blocks of this many pixels, and a block is skipped
 * when no column under it can be lit at the rows it crosses. What a block is
 * tested against is the reach of whole chunks of columns, at most
 * GFX_GLOW_REACH_CHUNKS of them however long the curve: all of it adds,
 * shifts and compares, since a 64-bit division is a library call here and
 * one per block cost more than the pixels it saved. */
#define GFX_GLOW_ROW_BLOCK    16
#define GFX_GLOW_REACH_CHUNKS 64

typedef struct {
    int32_t down_x, down_y;
} gfx_glow_pose_t;

/* What every pixel's distance is measured against: per column, the span it
 * covers and the rows its light can reach. The caller owns the four arrays,
 * `count` long; prepared once per change of the curve. */
typedef struct {
    int16_t* span_lo;
    int16_t* span_hi;
    int16_t* reach_lo;
    int16_t* reach_hi;
    int count;
    int band_lo, band_hi; /* the reach of the whole curve, Q4 */
    int chunk_shift;      /* a chunk is 1 << chunk_shift columns */
    int16_t chunk_lo[GFX_GLOW_REACH_CHUNKS];
    int16_t chunk_hi[GFX_GLOW_REACH_CHUNKS];
} gfx_glow_field_t;

/* The reach of each chunk of columns as one range, for gfx_glow_block_can_be_lit(). */
static inline void
gfx_glow_field_chunk(gfx_glow_field_t* field) {
    field->chunk_shift = 4;
    while (((field->count - 1) >> field->chunk_shift) >= GFX_GLOW_REACH_CHUNKS) {
        field->chunk_shift++;
    }
    for (int c = 0; c < GFX_GLOW_REACH_CHUNKS; c++) {
        field->chunk_lo[c] = INT16_MAX;
        field->chunk_hi[c] = INT16_MIN;
    }
    for (int x = 0; x < field->count; x++) {
        const int c = x >> field->chunk_shift;
        field->chunk_lo[c] = field->reach_lo[x] < field->chunk_lo[c] ? field->reach_lo[x] : field->chunk_lo[c];
        field->chunk_hi[c] = field->reach_hi[x] > field->chunk_hi[c] ? field->reach_hi[x] : field->chunk_hi[c];
    }
}

static inline void
gfx_glow_field_prepare(gfx_glow_field_t* field, const int16_t* y, const gfx_glow_style_t* style) {
    for (int x = 0; x < field->count; x++) {
        int lo, hi;
        gfx_glow_column_span(y, field->count, x, &lo, &hi);
        field->span_lo[x] = (int16_t)lo;
        field->span_hi[x] = (int16_t)hi;
    }
    field->band_lo = INT32_MAX;
    field->band_hi = INT32_MIN;
    for (int x = 0; x < field->count; x++) {
        int lo = INT32_MAX;
        int hi = INT32_MIN;
        for (int k = -style->radius; k <= style->radius; k++) {
            const int j = x + k;
            if (j < 0 || j >= field->count) {
                continue;
            }
            const int chord = style->chord[k < 0 ? -k : k];
            lo = field->span_lo[j] - chord < lo ? field->span_lo[j] - chord : lo;
            hi = field->span_hi[j] + chord > hi ? field->span_hi[j] + chord : hi;
        }
        field->reach_lo[x] = (int16_t)lo;
        field->reach_hi[x] = (int16_t)hi;
        field->band_lo = lo < field->band_lo ? lo : field->band_lo;
        field->band_hi = hi > field->band_hi ? hi : field->band_hi;
    }
    gfx_glow_field_chunk(field);
}

/* gfx_glow_distance2() for a point that is not on a column's centre. */
static inline int
gfx_glow_field_distance2(const gfx_glow_field_t* field, int radius, int vx, int vy) {
    const int own = vx >> GFX_GLOW_Q_SHIFT;
    const int off = vx - (own * GFX_GLOW_ONE + GFX_GLOW_ONE / 2);
    const int off_abs = off < 0 ? -off : off;
    int best = INT32_MAX;
    for (int k = 0; k <= radius; k++) {
        const int nearest = k * GFX_GLOW_ONE - off_abs;
        if (nearest > 0 && nearest * nearest >= best) {
            break;
        }
        for (int side = -1; side <= 1; side += 2) {
            const int j = own + side * k;
            if (j >= 0 && j < field->count) {
                const int across = side * k * GFX_GLOW_ONE - off;
                const int up = gfx_glow_outside(vy, field->span_lo[j], field->span_hi[j]);
                const int d2 = across * across + up * up;
                best = d2 < best ? d2 : best;
            }
            if (k == 0) {
                break;
            }
        }
    }
    return best;
}

static inline int64_t
gfx_glow_div_floor(int64_t n, int64_t d) {
    const int64_t q = n / d;
    return (n % d != 0 && ((n < 0) != (d < 0))) ? q - 1 : q;
}

/* Narrows [*a, *b) to the p for which lo <= v0 + p * step < hi. Empty comes
 * back as *b <= *a. */
static inline void
gfx_glow_narrow(int64_t v0, int64_t step, int64_t lo, int64_t hi, int* a, int* b) {
    if (step == 0) {
        if (v0 < lo || v0 >= hi) {
            *b = *a;
        }
        return;
    }
    /* first p at or past one bound, last p before the other */
    const int64_t enter = step > 0 ? lo - v0 : hi - 1 - v0;
    const int64_t leave = step > 0 ? hi - 1 - v0 : lo - v0;
    const int64_t first = -gfx_glow_div_floor(-enter, step);
    const int64_t last = gfx_glow_div_floor(leave, step);
    if (first > *a) {
        *a = first > *b ? *b : (int)first;
    }
    if (last + 1 < *b) {
        *b = last + 1 < *a ? *a : (int)(last + 1);
    }
}

/*
 * A map of the curve's light, so that drawing is a lookup. Searching beside
 * every pixel costs the radius, and so does the pixel count: a wide glow
 * cost its radius squared. Distance depends on the curve's shape and not on
 * how it is turned, so it is worked out once per shape, in the curve's own
 * frame, by an exact two-pass transform that costs the map's area whatever
 * the radius. Turning the curve then costs no distance work at all.
 */

/* One cell to this many pixels each way: a glow is smooth. The line itself
 * is not, so within GFX_GLOW_MAP_EXACT_PX of it a draw still searches, over
 * a window that small. */
#define GFX_GLOW_MAP_CELL     2
#define GFX_GLOW_MAP_EXACT_PX 5

/* A cell holds squared distance, which is what the ramp is indexed by and
 * interpolates almost exactly, in quarter pixels squared. */
#define GFX_GLOW_MAP_Q        2

typedef struct {
    uint16_t* cells; /* cols * rows, the caller's */
    int32_t* row_f;  /* scratch, cols long each */
    int32_t* row_z;
    int16_t* row_v;
    int cols, rows; /* what `cells` has room for */
    int lit_rows;   /* how many of them the last build filled: the band's */
    int origin_y;   /* view row of the top of cell row 0, set by the build */
    uint16_t far;   /* what a cell out of the light's reach holds */
} gfx_glow_map_t;

static inline int
gfx_glow_map_cols(int count) {
    return (count + GFX_GLOW_MAP_CELL - 1) / GFX_GLOW_MAP_CELL;
}

/* One row of the second pass: the lower envelope of the parabolas that the
 * first pass's vertical distances raise, after Felzenszwalb and
 * Huttenlocher, in integers. `step2` is the squared width of a cell. */
static inline void
gfx_glow_map_row(const gfx_glow_map_t* map, uint16_t* out, int step2) {
    int hulls = 0;
    for (int q = 0; q < map->cols; q++) {
        if (map->row_f[q] >= map->far) {
            continue;
        }
        const int64_t own = (int64_t)map->row_f[q] + (int64_t)step2 * q * q;
        int64_t meet = 0;
        while (hulls > 0) {
            const int v = map->row_v[hulls - 1];
            const int64_t other = (int64_t)map->row_f[v] + (int64_t)step2 * v * v;
            meet = gfx_glow_div_floor(own - other, (int64_t)2 * step2 * (q - v));
            if (meet > map->row_z[hulls - 1]) {
                break;
            }
            hulls--;
        }
        map->row_v[hulls] = (int16_t)q;
        map->row_z[hulls] = hulls == 0 ? INT32_MIN : (int32_t)meet;
        hulls++;
    }
    int k = 0;
    for (int p = 0; p < map->cols; p++) {
        if (hulls == 0) {
            out[p] = map->far;
            continue;
        }
        while (k + 1 < hulls && map->row_z[k + 1] < p) {
            k++;
        }
        const int v = map->row_v[k];
        const int64_t d2 = (int64_t)step2 * (p - v) * (p - v) + map->row_f[v];
        out[p] = d2 < map->far ? (uint16_t)d2 : map->far;
    }
}

static inline void
gfx_glow_map_build(gfx_glow_map_t* map, const gfx_glow_field_t* field, const gfx_glow_style_t* style) {
    const int to_map = GFX_GLOW_Q_SHIFT - GFX_GLOW_MAP_Q;
    const int reach = (style->radius + GFX_GLOW_MAP_CELL) << GFX_GLOW_MAP_Q;
    const int step = GFX_GLOW_MAP_CELL << GFX_GLOW_MAP_Q;
    map->far = (uint16_t)(reach * reach);
    map->origin_y = (field->band_lo >> GFX_GLOW_Q_SHIFT) - GFX_GLOW_MAP_CELL;
    const int band_px = ((field->band_hi - field->band_lo) >> GFX_GLOW_Q_SHIFT) + 3 * GFX_GLOW_MAP_CELL;
    const int band_rows = band_px / GFX_GLOW_MAP_CELL + 1;
    map->lit_rows = band_rows < map->rows ? band_rows : map->rows;

    for (int row = 0; row < map->lit_rows; row++) {
        const int top = (map->origin_y + row * GFX_GLOW_MAP_CELL) << GFX_GLOW_Q_SHIFT;
        const int centre = top + (GFX_GLOW_MAP_CELL << GFX_GLOW_Q_SHIFT) / 2;
        for (int c = 0; c < map->cols; c++) {
            int nearest = INT32_MAX;
            for (int j = c * GFX_GLOW_MAP_CELL; j < (c + 1) * GFX_GLOW_MAP_CELL && j < field->count; j++) {
                const int up = gfx_glow_outside(centre, field->span_lo[j], field->span_hi[j]) >> to_map;
                nearest = up < nearest ? up : nearest;
            }
            map->row_f[c] = nearest < reach ? nearest * nearest : map->far;
        }
        gfx_glow_map_row(map, map->cells + (size_t)row * map->cols, step * step);
    }
}

/* Squared distance at a view position, Q8 like gfx_glow_distance2(), from
 * the four cells around it. Above and below the map there is no light; past
 * either end the end cells stand. */
static inline int
gfx_glow_map_distance2(const gfx_glow_map_t* map, int x_q4, int y_q4) {
    const int cell_q4 = GFX_GLOW_MAP_CELL << GFX_GLOW_Q_SHIFT;
    const int to_q8 = 2 * (GFX_GLOW_Q_SHIFT - GFX_GLOW_MAP_Q);
    const int u = x_q4 - cell_q4 / 2;
    const int v = y_q4 - (map->origin_y << GFX_GLOW_Q_SHIFT) - cell_q4 / 2;
    int cy = v / cell_q4;
    if (v < 0 || cy + 1 >= map->lit_rows) {
        return (int)map->far << to_q8;
    }
    int cx = u < 0 ? 0 : u / cell_q4;
    int fx = u < 0 ? 0 : u % cell_q4;
    if (cx + 1 >= map->cols) {
        cx = map->cols - 2;
        fx = cell_q4;
    }
    const int fy = v % cell_q4;
    const uint16_t* upper = map->cells + (size_t)cy * map->cols + cx;
    const uint16_t* lower = upper + map->cols;
    const int along_upper = upper[0] * (cell_q4 - fx) + upper[1] * fx;
    const int along_lower = lower[0] * (cell_q4 - fx) + lower[1] * fx;
    const int64_t blended = (int64_t)along_upper * (cell_q4 - fy) + (int64_t)along_lower * fy;
    return (int)(blended / (cell_q4 * cell_q4)) << to_q8;
}

/* The distance a posed draw colours a pixel by: from the map where there is
 * one and the pixel is clear of the line, searched otherwise. */
static inline int
gfx_glow_posed_distance2(const gfx_glow_field_t* field, const gfx_glow_map_t* map, int radius, int x_q4, int y_q4) {
    if (map == NULL) {
        return gfx_glow_field_distance2(field, radius, x_q4, y_q4);
    }
    const int exact = GFX_GLOW_MAP_EXACT_PX * GFX_GLOW_ONE;
    const int mapped = gfx_glow_map_distance2(map, x_q4, y_q4);
    if (mapped >= exact * exact) {
        return mapped;
    }
    return gfx_glow_field_distance2(field, GFX_GLOW_MAP_EXACT_PX + 1, x_q4, y_q4);
}

/* A colour dimmed to `keep`/256 of itself, each channel rounded down so
 * that repeated dimming reaches black rather than sticking one step above.
 * Green is dimmed at red and blue's five bits: at its own six it outlives
 * them, and a white trail fades through green. */
static inline gfx_color_t
gfx_glow_dim(gfx_color_t colour, int keep) {
    const uint32_t c = (uint16_t)((colour >> 8) | (colour << 8));
    const uint32_t r = (((c >> 11) & 0x1F) * (uint32_t)keep) >> 8;
    const uint32_t g5 = (((c >> 6) & 0x1F) * (uint32_t)keep) >> 8;
    const uint32_t b = ((c & 0x1F) * (uint32_t)keep) >> 8;
    const uint32_t dimmed = (r << 11) | (g5 << 6) | ((g5 >> 4) << 5) | b;
    return (gfx_color_t)((dimmed >> 8) | (dimmed << 8));
}

/* How many draws dim the brightest colour to black at this `trail`: how long
 * a fading tail has to go on being drawn after the curve last moved. Counted
 * and not watched for on the panel, where the tail shares its rows with
 * whatever else is drawn there and lit pixels that are not its own never
 * run out. */
static inline int
gfx_glow_trail_draws(int trail) {
    if (trail <= 0 || trail >= 255) {
        return 0;
    }
    int draws = 0;
    for (uint32_t level = 0x1F; level != 0; level = (level * (uint32_t)trail) >> 8) {
        draws++;
    }
    return draws;
}

/* How much light a colour is, for telling which of two is brighter. */
static inline int
gfx_glow_light(gfx_color_t colour) {
    const uint32_t c = (uint16_t)((colour >> 8) | (colour << 8));
    return (int)(((c >> 11) & 0x1F) * 2 + ((c >> 5) & 0x3F) + (c & 0x1F) * 2);
}

/* Colours [a, b) of one panel row against the exact per-column reach test -
 * the truth a caller's superset is narrowed toward. Folds into new_lo/new_hi
 * so several calls across one row still cover its whole lit span. */
static inline void
gfx_glow_posed_row_light(gfx_color_t* dst, const gfx_glow_field_t* field, const gfx_glow_map_t* map,
                         const gfx_glow_style_t* style, int trail, int py, int to_q4, int64_t vx, int64_t vy,
                         int64_t right_x, int64_t down_x, int a, int b, int* new_lo, int* new_hi) {
    for (int px = a; px < b; px++, vx += right_x, vy += down_x) {
        const int x_q4 = (int)(vx >> to_q4);
        const int y_q4 = (int)(vy >> to_q4);
        const int column = x_q4 >> GFX_GLOW_Q_SHIFT;
        if (column < 0 || column >= field->count || y_q4 < field->reach_lo[column] || y_q4 > field->reach_hi[column]) {
            continue;
        }
        const gfx_color_t colour =
            gfx_glow_colour(style, gfx_glow_posed_distance2(field, map, style->radius, x_q4, y_q4), px, py);
        if (colour == GFX_RGB(0x000000)) {
            continue;
        }
        /* Each new band overlaps most of the last, and would overwrite its
         * bright core with a dim rim: what was left behind would be rim
         * light only. A trail keeps whichever is brighter. */
        if (trail > 0 && gfx_glow_light(dst[px]) > gfx_glow_light(colour)) {
            continue;
        }
        dst[px] = colour;
        *new_lo = *new_hi > *new_lo ? *new_lo : px;
        *new_hi = px + 1;
    }
}

/* A row that crosses only a few view columns - the curve running along the
 * panel's rows, or nearly - is narrowed once, to the exact reach of just
 * those columns: tighter than any block, for one division a row. */
#define GFX_GLOW_FEW_COLUMNS 4

static inline void
gfx_glow_posed_row_few_columns(gfx_color_t* dst, const gfx_glow_field_t* field, const gfx_glow_map_t* map,
                               const gfx_glow_style_t* style, int trail, int py, int to_q4, int64_t vx0, int64_t vy0,
                               int64_t right_x, int64_t down_x, int col_lo, int col_hi, int a, int b, int* new_lo,
                               int* new_hi) {
    int lo = field->reach_lo[col_lo];
    int hi = field->reach_hi[col_lo];
    for (int x = col_lo + 1; x <= col_hi; x++) {
        lo = field->reach_lo[x] < lo ? field->reach_lo[x] : lo;
        hi = field->reach_hi[x] > hi ? field->reach_hi[x] : hi;
    }
    gfx_glow_narrow(vy0, down_x, (int64_t)lo << to_q4, ((int64_t)hi + 1) << to_q4, &a, &b);
    if (b > a) {
        gfx_glow_posed_row_light(dst, field, map, style, trail, py, to_q4, vx0 + (int64_t)a * right_x,
                                 vy0 + (int64_t)a * down_x, right_x, down_x, a, b, new_lo, new_hi);
    }
}

/* Whether any pixel of a row between two of its pixels can be lit, from
 * their view positions alone. View x and view y both run one way along a row
 * and a shift keeps their order, so every pixel between lies in the columns
 * and the rows between the two ends: nothing outside the reach of those
 * columns' chunks can pass the per-column test. */
static inline bool
gfx_glow_block_can_be_lit(const gfx_glow_field_t* field, int to_q4, int64_t vx_a, int64_t vx_b, int64_t vy_a,
                          int64_t vy_b) {
    const int shift = 14 + field->chunk_shift;
    const int chunk_a = (int)(vx_a >> shift);
    const int chunk_b = (int)(vx_b >> shift);
    const int y_a = (int)(vy_a >> to_q4);
    const int y_b = (int)(vy_b >> to_q4);
    const int y_lo = y_a < y_b ? y_a : y_b;
    const int y_hi = y_a < y_b ? y_b : y_a;
    for (int c = chunk_a < chunk_b ? chunk_a : chunk_b; c <= (chunk_a < chunk_b ? chunk_b : chunk_a); c++) {
        if (y_hi >= field->chunk_lo[c] && y_lo <= field->chunk_hi[c]) {
            return true;
        }
    }
    return false;
}

static inline void
gfx_glow_posed_row_blocks(gfx_color_t* dst, const gfx_glow_field_t* field, const gfx_glow_map_t* map,
                          const gfx_glow_style_t* style, int trail, int py, int to_q4, int64_t vx0, int64_t vy0,
                          int64_t right_x, int64_t down_x, int a, int b, int* new_lo, int* new_hi) {
    int64_t vx = vx0 + (int64_t)a * right_x;
    int64_t vy = vy0 + (int64_t)a * down_x;
    for (int p = a; p < b; p += GFX_GLOW_ROW_BLOCK) {
        const int q = p + GFX_GLOW_ROW_BLOCK < b ? p + GFX_GLOW_ROW_BLOCK : b;
        const int64_t last = q - 1 - p;
        if (gfx_glow_block_can_be_lit(field, to_q4, vx, vx + last * right_x, vy, vy + last * down_x)) {
            gfx_glow_posed_row_light(dst, field, map, style, trail, py, to_q4, vx, vy, right_x, down_x, p, q, new_lo,
                                     new_hi);
        }
        vx += GFX_GLOW_ROW_BLOCK * right_x;
        vy += GFX_GLOW_ROW_BLOCK * down_x;
    }
}

/*
 * Draws panel rows [row0, row1) of the posed curve. `lit_lo`/`lit_hi` hold,
 * per panel row, the stretch still lit from earlier draws, and `trail` is
 * what becomes of it first: 0 blackens it, 255 leaves it, and between it is
 * dimmed to trail/256, a tail that is gone after gfx_glow_trail_draws() more
 * draws. `map` is the curve's gfx_glow_map_t, or NULL to search for every
 * pixel. Returns the panel box touched.
 */
static inline gfx_glow_box_t
gfx_glow_draw_posed_rows(gfx_target_t target, int clip_x0, int clip_y0, int clip_x1, int clip_y1, int panel_w,
                         int panel_h, const gfx_glow_field_t* field, const gfx_glow_map_t* map, int view_h,
                         gfx_glow_pose_t pose, int row0, int row1, int16_t* lit_lo, int16_t* lit_hi, int trail,
                         const gfx_glow_style_t* style) {
    gfx_glow_box_t box = {0, 0, 0, 0};
    const int64_t right_x = pose.down_y;
    const int64_t right_y = -pose.down_x;
    /* Centres doubled, so that a half pixel stays an integer. */
    const int64_t view_cx2 = field->count - 1;
    const int64_t view_cy2 = view_h - 1;
    const int64_t half_q14 = GFX_GLOW_POSE_ONE / 2;
    const int to_q4 = 14 - GFX_GLOW_Q_SHIFT;

    row0 = row0 < clip_y0 ? clip_y0 : row0;
    row0 = row0 < target.y0 ? target.y0 : row0;
    row1 = row1 > clip_y1 ? clip_y1 : row1;
    row1 = row1 > target.y0 + target.height ? target.y0 + target.height : row1;

    for (int py = row0; py < row1; py++) {
        gfx_color_t* dst = gfx_target_row(target, py);
        int kept_lo = 0;
        int kept_hi = 0;
        for (int px = lit_lo[py]; px < lit_hi[py]; px++) {
            if (trail < 255) {
                dst[px] = trail == 0 ? GFX_RGB(0x000000) : gfx_glow_dim(dst[px], trail);
            }
            if (dst[px] != GFX_RGB(0x000000)) {
                kept_lo = kept_hi > kept_lo ? kept_lo : px;
                kept_hi = px + 1;
            }
        }
        if (lit_hi[py] > lit_lo[py]) {
            gfx_glow_box_add(&box, lit_lo[py], py);
            gfx_glow_box_add(&box, lit_hi[py] - 1, py);
        }

        /* The view position of this row's pixel 0 centre, Q14, and its step. */
        const int64_t dx2 = -(int64_t)(panel_w - 1);
        const int64_t dy2 = 2 * (int64_t)py - (panel_h - 1);
        const int64_t vx0 = (view_cx2 * GFX_GLOW_POSE_ONE + dx2 * right_x + dy2 * right_y) / 2 + half_q14;
        const int64_t vy0 = (view_cy2 * GFX_GLOW_POSE_ONE + dx2 * pose.down_x + dy2 * pose.down_y) / 2 + half_q14;

        int a = clip_x0 < 0 ? 0 : clip_x0;
        int b = clip_x1 > panel_w ? panel_w : clip_x1;
        gfx_glow_narrow(vx0, right_x, 0, (int64_t)field->count << 14, &a, &b);

        int new_lo = 0;
        int new_hi = 0;
        if (a < b) {
            const int col_a = (int)((vx0 + (int64_t)a * right_x) >> 14);
            const int col_b = (int)((vx0 + (int64_t)(b - 1) * right_x) >> 14);
            const int col_lo = col_a < col_b ? col_a : col_b;
            const int col_hi = col_a < col_b ? col_b : col_a;
            if (col_hi - col_lo < GFX_GLOW_FEW_COLUMNS) {
                gfx_glow_posed_row_few_columns(dst, field, map, style, trail, py, to_q4, vx0, vy0, right_x, pose.down_x,
                                               col_lo, col_hi, a, b, &new_lo, &new_hi);
            } else {
                gfx_glow_posed_row_blocks(dst, field, map, style, trail, py, to_q4, vx0, vy0, right_x, pose.down_x, a,
                                          b, &new_lo, &new_hi);
            }
        }
        const bool has_kept = kept_hi > kept_lo;
        const bool has_new = new_hi > new_lo;
        lit_lo[py] = (int16_t)(has_kept && (!has_new || kept_lo < new_lo) ? kept_lo : new_lo);
        lit_hi[py] = (int16_t)(has_kept && (!has_new || kept_hi > new_hi) ? kept_hi : new_hi);
        if (new_hi > new_lo) {
            gfx_glow_box_add(&box, new_lo, py);
            gfx_glow_box_add(&box, new_hi - 1, py);
        }
    }
    return box;
}
