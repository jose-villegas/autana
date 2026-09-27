#include "span_raster.h"

#include <stdbool.h>

#include "fast_float.h"

#pragma GCC diagnostic error "-Wdouble-promotion"

/* 65535 in 16.16: inverse depth 1.0 lands on the largest 16-bit depth. */
#define DEPTH_ONE 4294901760.0f
#define COLOR_ONE 65536.0f
#define COLOR_MAX (255.0f * COLOR_ONE)

/* Every attribute already scaled to its fixed-point range; only the
 * per-pixel step is also kept as an integer. */
typedef struct {
    float x0, y0;
    float at[4];   /* z, r, g, b at (x0, y0) */
    float ddx[4];  /* per pixel along a row */
    float ddy[4];  /* per row */
    int32_t dx[4]; /* ddx in fixed point */
} gradients_t;

static const float attribute_max[4] = {DEPTH_ONE, COLOR_MAX, COLOR_MAX, COLOR_MAX};

/* A sliver's depth gradient can pass int32's range; the clamped step is
 * wrong only by what a span that short never reaches. */
static inline int32_t
to_step(float step) {
    return (int32_t)(step < -2.0e9f ? -2.0e9f : (step > 2.0e9f ? 2.0e9f : step));
}

static bool
compute_gradients(const span_vertex_t* a, const span_vertex_t* b, const span_vertex_t* c, gradients_t* out) {
    const float e1x = b->x - a->x, e1y = b->y - a->y;
    const float e2x = c->x - a->x, e2y = c->y - a->y;
    const float area2 = e1x * e2y - e2x * e1y;
    if (area2 > -1e-6f && area2 < 1e-6f) {
        return false;
    }
    const float inv = 1.0f / area2;
    const float va[4] = {a->z * DEPTH_ONE, a->r * COLOR_ONE, a->g * COLOR_ONE, a->b * COLOR_ONE};
    const float vb[4] = {b->z * DEPTH_ONE, b->r * COLOR_ONE, b->g * COLOR_ONE, b->b * COLOR_ONE};
    const float vc[4] = {c->z * DEPTH_ONE, c->r * COLOR_ONE, c->g * COLOR_ONE, c->b * COLOR_ONE};
    for (int k = 0; k < 4; k++) {
        const float d1 = vb[k] - va[k], d2 = vc[k] - va[k];
        out->ddx[k] = (d1 * e2y - d2 * e1y) * inv;
        out->ddy[k] = (d2 * e1x - d1 * e2x) * inv;
        out->at[k] = va[k];
        out->dx[k] = to_step(out->ddx[k]);
    }
    out->x0 = a->x;
    out->y0 = a->y;
    return true;
}

static inline float
clampf(float v, float lo, float hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

static inline gfx_color_t
pack(int32_t r, int32_t g, int32_t b) {
    const uint32_t native = ((uint32_t)(r >> 8) & 0xF800u) | ((uint32_t)(g >> 13) & 0x07E0u) | ((uint32_t)b >> 19);
    return (gfx_color_t)((native >> 8) | (native << 8));
}

/* A pixel centre just outside the triangle extrapolates past the vertex
 * range, and a wrapped channel would be a wrong-coloured pixel. The start
 * is clamped; only a span whose end would still leave the range pays for
 * a clamped end and a step recomputed from both. */
static void
fill_span(const span_target_t* target, const gradients_t* g, int y, int x_first, int x_last) {
    const float yc = (float)y + 0.5f - g->y0;
    const float xs = (float)x_first + 0.5f - g->x0;
    const int count = x_last - x_first;

    int32_t v[4], d[4];
    for (int k = 0; k < 4; k++) {
        const float start = clampf(g->at[k] + g->ddx[k] * xs + g->ddy[k] * yc, 0.0f, attribute_max[k]);
        const float end = start + g->ddx[k] * (float)count;
        v[k] = (int32_t)(uint32_t)start;
        d[k] = g->dx[k];
        if (count > 0 && (end < 0.0f || end > attribute_max[k])) {
            d[k] = to_step((clampf(end, 0.0f, attribute_max[k]) - start) / (float)count);
        }
    }

    const int row = (y - target->row0) * target->width;
    uint16_t* depth = target->depth + row;
    gfx_color_t* color = target->color + row;
    uint32_t z = (uint32_t)v[0];
    int32_t r = v[1], gg = v[2], b = v[3];
    for (int x = x_first; x <= x_last; x++) {
        const uint16_t zq = (uint16_t)(z >> 16);
        if (zq > depth[x]) {
            depth[x] = zq;
            color[x] = pack(r, gg, b);
        }
        z += (uint32_t)d[0];
        r += d[1];
        gg += d[2];
        b += d[3];
    }
}

static void
fill_flat_span(const span_target_t* target, int y, int x_first, int x_last, uint16_t zq, gfx_color_t color) {
    const int row = (y - target->row0) * target->width;
    uint16_t* depth = target->depth + row;
    gfx_color_t* out = target->color + row;
    for (int x = x_first; x <= x_last; x++) {
        if (zq > depth[x]) {
            depth[x] = zq;
            out[x] = color;
        }
    }
}

/* Small enough that a gradient across it is invisible: one colour, one
 * depth. Its coverage is still decided by the same edge walk. */
#define FLAT_MAX_ROWS  2
#define FLAT_MAX_WIDTH 3.0f

static bool
is_tiny(const span_vertex_t* a, const span_vertex_t* b, const span_vertex_t* c, int rows) {
    if (rows > FLAT_MAX_ROWS) {
        return false;
    }
    const float lo = a->x < b->x ? (a->x < c->x ? a->x : c->x) : (b->x < c->x ? b->x : c->x);
    const float hi = a->x > b->x ? (a->x > c->x ? a->x : c->x) : (b->x > c->x ? b->x : c->x);
    return hi - lo <= FLAT_MAX_WIDTH;
}

void
span_raster_triangle(const span_target_t* target, const span_vertex_t* a, const span_vertex_t* b,
                     const span_vertex_t* c) {
    const span_vertex_t* v0 = a;
    const span_vertex_t* v1 = b;
    const span_vertex_t* v2 = c;
    const span_vertex_t* t;
    if (v1->y < v0->y) {
        t = v0, v0 = v1, v1 = t;
    }
    if (v2->y < v1->y) {
        t = v1, v1 = v2, v2 = t;
    }
    if (v1->y < v0->y) {
        t = v0, v0 = v1, v1 = t;
    }

    const float top = (float)target->row0 - 1.0f, bottom = (float)target->row1 + 1.0f;
    int y_first = fast_ceil(clampf(v0->y, top, bottom) - 0.5f);
    int y_end = fast_ceil(clampf(v2->y, top, bottom) - 0.5f);
    if (y_first < target->row0) {
        y_first = target->row0;
    }
    if (y_end > target->row1) {
        y_end = target->row1;
    }
    if (y_first >= y_end) {
        return; /* no pixel centre row inside this window */
    }

    const bool flat = is_tiny(a, b, c, y_end - y_first);
    gradients_t g;
    uint16_t flat_z = 0;
    gfx_color_t flat_color = 0;
    if (flat) {
        const float third = 1.0f / 3.0f;
        flat_z = (uint16_t)(clampf((a->z + b->z + c->z) * third, 0.0f, 1.0f) * 65535.0f);
        const float r = clampf((a->r + b->r + c->r) * third, 0.0f, 255.0f) * COLOR_ONE;
        const float gr = clampf((a->g + b->g + c->g) * third, 0.0f, 255.0f) * COLOR_ONE;
        const float bl = clampf((a->b + b->b + c->b) * third, 0.0f, 255.0f) * COLOR_ONE;
        flat_color = pack((int32_t)r, (int32_t)gr, (int32_t)bl);
    } else if (!compute_gradients(a, b, c, &g)) {
        return;
    }

    const float dy01 = v1->y - v0->y, dy12 = v2->y - v1->y, dy02 = v2->y - v0->y;
    const float inv02 = 1.0f / dy02;
    const float long_slope = (v2->x - v0->x) * inv02;
    const float top_slope = dy01 > 0.0f ? (v1->x - v0->x) / dy01 : 0.0f;
    const float bottom_slope = dy12 > 0.0f ? (v2->x - v1->x) / dy12 : 0.0f;
    const int last_column = target->width - 1;
    const float right_limit = (float)target->width + 1.0f;

    for (int y = y_first; y < y_end; y++) {
        const float yc = (float)y + 0.5f;
        const float x_long = v0->x + (yc - v0->y) * long_slope;
        const float x_short = yc < v1->y ? v0->x + (yc - v0->y) * top_slope : v1->x + (yc - v1->y) * bottom_slope;
        const float left = x_long < x_short ? x_long : x_short;
        const float right = x_long < x_short ? x_short : x_long;

        int x_first = fast_ceil(clampf(left, -1.0f, right_limit) - 0.5f);
        int x_last = fast_ceil(clampf(right, -1.0f, right_limit) - 0.5f) - 1;
        if (x_first < 0) {
            x_first = 0;
        }
        if (x_last > last_column) {
            x_last = last_column;
        }
        if (x_first > x_last) {
            continue;
        }
        if (flat) {
            fill_flat_span(target, y, x_first, x_last, flat_z, flat_color);
        } else {
            fill_span(target, &g, y, x_first, x_last);
        }
    }
}
