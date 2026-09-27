#include "render/r3d_span.h"

#include <stdbool.h>

#include "util/fast_float.h"

#pragma GCC diagnostic error "-Wdouble-promotion"

/* Attributes run in fixed point: depth as 16.8 (65535 is nearest), colour
 * channels as 8.8, whose steepest real step - all 255 levels in one pixel -
 * is far below the clamp. Only a sliver's depth step can reach 2^22; with
 * that bound and at most a screen of steps from the triangle's own corner,
 * every sum stays inside int32. */
#define ATTRIBUTES  4
#define DEPTH_SCALE 16776960.0f /* 65535 << 8 */
#define COLOR_SCALE 256.0f
#define STEP_MAX    4194304.0f

static const int32_t value_max[ATTRIBUTES] = {16776960, 65280, 65280, 65280};

int r3d_span_stop_after;

typedef struct {
    int32_t base[ATTRIBUTES]; /* at the centre of pixel (x_origin, the triangle's first row) */
    int32_t dx[ATTRIBUTES];
    int32_t dy[ATTRIBUTES];
    int x_origin;
} gradients_t;

static inline float
clampf(float v, float lo, float hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

static inline int32_t
to_step(float step) {
    return (int32_t)clampf(step, -STEP_MAX, STEP_MAX);
}

static inline int32_t
clamp_value(int32_t v, int32_t max) {
    return v < 0 ? 0 : (v > max ? max : v);
}

static inline uint16_t
pack(int32_t r, int32_t g, int32_t b) {
    const uint32_t native = ((uint32_t)r & 0xF800u) | (((uint32_t)g >> 5) & 0x07E0u) | ((uint32_t)b >> 11);
    return (uint16_t)((native >> 8) | (native << 8));
}

static bool
compute_gradients(const r3d_span_vertex_t* a, const r3d_span_vertex_t* b, const r3d_span_vertex_t* c, int x_origin,
                  int y_anchor, gradients_t* out) {
    const float e1x = b->x - a->x, e1y = b->y - a->y;
    const float e2x = c->x - a->x, e2y = c->y - a->y;
    const float area2 = e1x * e2y - e2x * e1y;
    if (area2 > -1e-6f && area2 < 1e-6f) {
        return false;
    }
    const float inv = 1.0f / area2;
    const float va[ATTRIBUTES] = {a->z * DEPTH_SCALE, a->r * COLOR_SCALE, a->g * COLOR_SCALE, a->b * COLOR_SCALE};
    const float vb[ATTRIBUTES] = {b->z * DEPTH_SCALE, b->r * COLOR_SCALE, b->g * COLOR_SCALE, b->b * COLOR_SCALE};
    const float vc[ATTRIBUTES] = {c->z * DEPTH_SCALE, c->r * COLOR_SCALE, c->g * COLOR_SCALE, c->b * COLOR_SCALE};
    const float ox = (float)x_origin + 0.5f - a->x, oy = (float)y_anchor + 0.5f - a->y;
    for (int k = 0; k < ATTRIBUTES; k++) {
        const float d1 = vb[k] - va[k], d2 = vc[k] - va[k];
        const float ddx = (d1 * e2y - d2 * e1y) * inv;
        const float ddy = (d2 * e1x - d1 * e2x) * inv;
        out->dx[k] = to_step(ddx);
        out->dy[k] = to_step(ddy);
        out->base[k] = (int32_t)clampf(va[k] + ddx * ox + ddy * oy, -2.0e9f, 2.0e9f);
    }
    out->x_origin = x_origin;
    return true;
}

/* A pixel centre just outside the triangle extrapolates past the vertex
 * range, and a wrapped channel would be a wrong-coloured pixel. Each span's
 * start is clamped; only a span whose end would still leave the range pays
 * for a step recomputed from both clamped ends. */
static void
fill_span(const r3d_span_target_t* target, const gradients_t* g, const int32_t row[ATTRIBUTES], int y, int x_first,
          int x_last) {
    const int32_t offset = x_first - g->x_origin;
    const int count = x_last - x_first;
    int32_t v[ATTRIBUTES], d[ATTRIBUTES];
    for (int k = 0; k < ATTRIBUTES; k++) {
        const int32_t start = clamp_value(row[k] + g->dx[k] * offset, value_max[k]);
        const int32_t end = start + g->dx[k] * count;
        v[k] = start;
        d[k] = g->dx[k];
        if (count > 0 && (end < 0 || end > value_max[k])) {
            d[k] = (clamp_value(end, value_max[k]) - start) / count;
        }
    }
    if (r3d_span_stop_after == 3) {
        return;
    }

    const int row_offset = (y - target->row0) * target->width;
    uint16_t* depth = target->depth + row_offset;
    uint16_t* color = target->color + row_offset;
    int32_t z = v[0], r = v[1], gg = v[2], b = v[3];
    for (int x = x_first; x <= x_last; x++) {
        const uint16_t zq = (uint16_t)(z >> 8);
        if (zq > depth[x]) {
            depth[x] = zq;
            color[x] = pack(r, gg, b);
        }
        z += d[0];
        r += d[1];
        gg += d[2];
        b += d[3];
    }
}

static void
fill_flat_span(const r3d_span_target_t* target, int y, int x_first, int x_last, uint16_t zq, uint16_t color) {
    const int row = (y - target->row0) * target->width;
    uint16_t* depth = target->depth + row;
    uint16_t* out = target->color + row;
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

/* An edge in 16.16, anchored at its own first row whatever window is being
 * drawn: both triangles sharing it, and any window of rows, get the same
 * position on every row, so there are no gaps and no double fills. 64 bits,
 * because a triangle just past the near plane reaches far off screen. */
typedef struct {
    int64_t x;
    int64_t step;
} edge_t;

static edge_t
edge_at(const r3d_span_vertex_t* top, const r3d_span_vertex_t* bottom, int y) {
    const float dy = bottom->y - top->y;
    const float slope = dy > 0.0f ? (bottom->x - top->x) / dy : 0.0f;
    const int anchor = fast_ceil(top->y - 0.5f);
    const float x = top->x + ((float)anchor + 0.5f - top->y) * slope;
    const edge_t e = {(int64_t)(x * 65536.0f), (int64_t)(slope * 65536.0f)};
    return y == anchor ? e : (edge_t){e.x + (int64_t)(y - anchor) * e.step, e.step};
}

/* ceil(x - 0.5) of a 16.16 position: the first pixel whose centre is at or
 * right of it. */
static inline int
first_pixel(int64_t x) {
    return (int)((x + 0x7FFF) >> 16);
}

static inline int
first_pixel32(int32_t x) {
    return (x + 0x7FFF) >> 16;
}

/* Inside this, edge positions fit 32 bits; a triangle reaching past it (only
 * just past the near plane) walks in 64. */
#define NARROW_LIMIT    8192.0f

/* A step this steep means an edge under one row tall: it is never taken
 * before the edge ends, so clamping it changes nothing. */
#define NARROW_STEP_MAX 1073741824

typedef struct {
    bool flat;
    uint16_t flat_z;
    uint16_t flat_color;
    const gradients_t* g;
} fill_t;

static inline void
fill_row(const r3d_span_target_t* target, const fill_t* f, const int32_t row[ATTRIBUTES], int y, int x_first,
         int x_last) {
    const int last_column = target->width - 1;
    x_first = x_first < 0 ? 0 : x_first;
    x_last = x_last > last_column ? last_column : x_last;
    if (x_first > x_last || r3d_span_stop_after == 2) {
        return;
    }
    if (f->flat) {
        fill_flat_span(target, y, x_first, x_last, f->flat_z, f->flat_color);
    } else {
        fill_span(target, f->g, row, y, x_first, x_last);
    }
}

static inline void
step_row(const fill_t* f, int32_t row[ATTRIBUTES]) {
    if (!f->flat) {
        for (int k = 0; k < ATTRIBUTES; k++) {
            row[k] += f->g->dy[k];
        }
    }
}

typedef struct {
    int32_t x;
    int32_t step;
} edge32_t;

/* edge_at() in 32 bits, for vertices inside NARROW_LIMIT: the same float
 * arithmetic, so the same positions, without 64-bit conversions - each of
 * which is a library call on this chip. */
static edge32_t
edge32_at(const r3d_span_vertex_t* top, const r3d_span_vertex_t* bottom, int y) {
    const float dy = bottom->y - top->y;
    const float slope = dy > 0.0f ? (bottom->x - top->x) / dy : 0.0f;
    const int anchor = fast_ceil(top->y - 0.5f);
    const float x = top->x + ((float)anchor + 0.5f - top->y) * slope;
    const edge32_t e = {(int32_t)(x * 65536.0f),
                        (int32_t)clampf(slope * 65536.0f, (float)-NARROW_STEP_MAX, (float)NARROW_STEP_MAX)};
    return y == anchor ? e : (edge32_t){e.x + (y - anchor) * e.step, e.step};
}

static void
walk32(const r3d_span_target_t* target, const fill_t* f, int32_t row[ATTRIBUTES], int y0, int y1, edge32_t left,
       edge32_t right) {
    int32_t lx = left.x, rx = right.x;
    for (int y = y0; y < y1; y++) {
        fill_row(target, f, row, y, first_pixel32(lx), first_pixel32(rx) - 1);
        lx += left.step;
        rx += right.step;
        step_row(f, row);
    }
}

static void
walk64(const r3d_span_target_t* target, const fill_t* f, int32_t row[ATTRIBUTES], int y0, int y1, edge_t left,
       edge_t right) {
    for (int y = y0; y < y1; y++) {
        fill_row(target, f, row, y, first_pixel(left.x), first_pixel(right.x) - 1);
        left.x += left.step;
        right.x += right.step;
        step_row(f, row);
    }
}

/* Walks rows [y0, y1) between the edges top-a..bottom-a and top-b..bottom-b,
 * `a_on_left` saying which is which. */
static void
walk(const r3d_span_target_t* target, const fill_t* f, int32_t row[ATTRIBUTES], int y0, int y1,
     const r3d_span_vertex_t* top_a, const r3d_span_vertex_t* bottom_a, const r3d_span_vertex_t* top_b,
     const r3d_span_vertex_t* bottom_b, bool a_on_left, bool narrow) {
    if (y0 >= y1) {
        return;
    }
    if (narrow) {
        const edge32_t a = edge32_at(top_a, bottom_a, y0), b = edge32_at(top_b, bottom_b, y0);
        walk32(target, f, row, y0, y1, a_on_left ? a : b, a_on_left ? b : a);
    } else {
        const edge_t a = edge_at(top_a, bottom_a, y0), b = edge_at(top_b, bottom_b, y0);
        walk64(target, f, row, y0, y1, a_on_left ? a : b, a_on_left ? b : a);
    }
}

void
r3d_span_triangle(const r3d_span_target_t* target, const r3d_span_vertex_t* a, const r3d_span_vertex_t* b,
                  const r3d_span_vertex_t* c) {
    const r3d_span_vertex_t* v0 = a;
    const r3d_span_vertex_t* v1 = b;
    const r3d_span_vertex_t* v2 = c;
    const r3d_span_vertex_t* t;
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

    const float lo_x = a->x < b->x ? (a->x < c->x ? a->x : c->x) : (b->x < c->x ? b->x : c->x);
    const float hi_x = a->x > b->x ? (a->x > c->x ? a->x : c->x) : (b->x > c->x ? b->x : c->x);
    const bool flat = y_end - y_first <= FLAT_MAX_ROWS && hi_x - lo_x <= FLAT_MAX_WIDTH;

    /* Attributes anchor at the triangle's first row, or at screen row 0 for
     * one starting above the screen: never at a window's own edge. */
    const int y_anchor = fast_ceil(clampf(v0->y, -1.0f, bottom) - 0.5f);
    int x_origin = fast_ceil(lo_x - 0.5f);
    x_origin = x_origin < 0 ? 0 : x_origin;
    gradients_t g = {0};
    uint16_t flat_z = 0;
    uint16_t flat_color = 0;
    if (flat) {
        const float third = 1.0f / 3.0f;
        flat_z = (uint16_t)(clampf((a->z + b->z + c->z) * third, 0.0f, 1.0f) * 65535.0f);
        flat_color = pack((int32_t)(clampf((a->r + b->r + c->r) * third, 0.0f, 255.0f) * COLOR_SCALE),
                          (int32_t)(clampf((a->g + b->g + c->g) * third, 0.0f, 255.0f) * COLOR_SCALE),
                          (int32_t)(clampf((a->b + b->b + c->b) * third, 0.0f, 255.0f) * COLOR_SCALE));
    } else if (!compute_gradients(a, b, c, x_origin, y_anchor, &g)) {
        return;
    }
    if (r3d_span_stop_after == 1) {
        return;
    }

    /* The long edge v0-v2 runs the whole height, on the same side all the
     * way down; the short side is v0-v1 above v1 and v1-v2 below it. */
    const int y_mid = fast_ceil(clampf(v1->y, top, bottom) - 0.5f);
    const int split = y_mid < y_first ? y_first : (y_mid > y_end ? y_end : y_mid);
    const float long_x_at_v1 = v2->y > v0->y ? v0->x + (v1->y - v0->y) * (v2->x - v0->x) / (v2->y - v0->y) : v0->x;
    const bool long_on_left = long_x_at_v1 < v1->x;
    const bool narrow = lo_x > -NARROW_LIMIT && hi_x < NARROW_LIMIT && v0->y > -NARROW_LIMIT && v2->y < NARROW_LIMIT;

    int32_t row[ATTRIBUTES] = {0};
    if (!flat) {
        for (int k = 0; k < ATTRIBUTES; k++) {
            row[k] = y_first == y_anchor ? g.base[k] : (int32_t)(g.base[k] + (int64_t)(y_first - y_anchor) * g.dy[k]);
        }
    }
    const fill_t f = {flat, flat_z, flat_color, &g};

    walk(target, &f, row, y_first, split, v0, v2, v0, v1, long_on_left, narrow);
    walk(target, &f, row, split, y_end, v0, v2, v1, v2, long_on_left, narrow);
}
