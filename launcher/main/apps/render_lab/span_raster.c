#include "span_raster.h"

#include <stdbool.h>

#include "fast_float.h"

#pragma GCC diagnostic error "-Wdouble-promotion"

/* Every attribute runs in fixed point with 24 bits of range: depth as
 * 16.8 (65535 is nearest), colour channels as 8.16. A per-pixel or per-row
 * step is clamped to 2^21, far past any triangle big enough to show one;
 * with values bounded by 2^24 and at most a screen of steps from the
 * triangle's own corner, every sum stays inside int32. */
#define ATTRIBUTES        4
#define DEPTH_SCALE       16776960.0f /* 65535 << 8 */
#define COLOR_SCALE       65536.0f
#define VALUE_MAX         16776960 /* both depth and 255 << 16 */
#define STEP_MAX          2097152.0f
/* Beyond this a vertex takes the float path: 16.16 edge positions, and an
 * edge's whole run of steps, would overflow. */
#define FIXED_COORD_LIMIT 8192.0f

int span_raster_stop_after;

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
clamp_value(int32_t v) {
    return v < 0 ? 0 : (v > VALUE_MAX ? VALUE_MAX : v);
}

static inline gfx_color_t
pack(int32_t r, int32_t g, int32_t b) {
    const uint32_t native = ((uint32_t)(r >> 8) & 0xF800u) | ((uint32_t)(g >> 13) & 0x07E0u) | ((uint32_t)b >> 19);
    return (gfx_color_t)((native >> 8) | (native << 8));
}

static bool
compute_gradients(const span_vertex_t* a, const span_vertex_t* b, const span_vertex_t* c, int x_origin, int y_anchor,
                  gradients_t* out) {
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
fill_span(const span_target_t* target, const gradients_t* g, const int32_t row[ATTRIBUTES], int y, int x_first,
          int x_last) {
    const int32_t offset = x_first - g->x_origin;
    const int count = x_last - x_first;
    int32_t v[ATTRIBUTES], d[ATTRIBUTES];
    for (int k = 0; k < ATTRIBUTES; k++) {
        const int32_t start = clamp_value(row[k] + g->dx[k] * offset);
        const int32_t end = start + g->dx[k] * count;
        v[k] = start;
        d[k] = g->dx[k];
        if (count > 0 && (end < 0 || end > VALUE_MAX)) {
            d[k] = (clamp_value(end) - start) / count;
        }
    }
    if (span_raster_stop_after == 3) {
        return;
    }

    const int row_offset = (y - target->row0) * target->width;
    uint16_t* depth = target->depth + row_offset;
    gfx_color_t* color = target->color + row_offset;
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

/* An edge in 16.16, anchored at its own first row whatever window is being
 * drawn: both triangles sharing it, and any window of rows, get the same
 * position on every row, so there are no gaps and no double fills. */
typedef struct {
    int32_t x;
    int32_t step;
} edge_t;

static edge_t
edge_at(const span_vertex_t* top, const span_vertex_t* bottom, int y) {
    const float dy = bottom->y - top->y;
    const float slope = dy > 0.0f ? (bottom->x - top->x) / dy : 0.0f;
    const int anchor = fast_ceil(top->y - 0.5f);
    const float x = top->x + ((float)anchor + 0.5f - top->y) * slope;
    const edge_t e = {(int32_t)(x * 65536.0f), (int32_t)clampf(slope * 65536.0f, -1.0e9f, 1.0e9f)};
    /* Within FIXED_COORD_LIMIT the product is a displacement along the edge
     * itself, so it fits. */
    return (edge_t){e.x + (y - anchor) * e.step, e.step};
}

/* ceil(x - 0.5) of a 16.16 position: the first pixel whose centre is at or
 * right of it. */
static inline int
first_pixel(int32_t x) {
    return (x + 0x7FFF) >> 16;
}

/* The float path, for a triangle reaching past FIXED_COORD_LIMIT - rare,
 * only just past the near plane. */
/* 65535 in 16.16: inverse depth 1.0 lands on the largest 16-bit depth. */
#define FLOAT_DEPTH_ONE 4294901760.0f
#define FLOAT_COLOR_ONE 65536.0f
#define FLOAT_COLOR_MAX (255.0f * FLOAT_COLOR_ONE)

/* Every attribute already scaled to its fixed-point range; only the
 * per-pixel step is also kept as an integer. */
typedef struct {
    float x0, y0;
    float at[4];   /* z, r, g, b at (x0, y0) */
    float ddx[4];  /* per pixel along a row */
    float ddy[4];  /* per row */
    int32_t dx[4]; /* ddx in fixed point */
} float_gradients_t;

static const float float_attribute_max[4] = {FLOAT_DEPTH_ONE, FLOAT_COLOR_MAX, FLOAT_COLOR_MAX, FLOAT_COLOR_MAX};

/* A sliver's depth gradient can pass int32's range; the clamped step is
 * wrong only by what a span that short never reaches. */
static inline int32_t
float_to_step(float step) {
    return (int32_t)(step < -2.0e9f ? -2.0e9f : (step > 2.0e9f ? 2.0e9f : step));
}

static bool
float_compute_gradients(const span_vertex_t* a, const span_vertex_t* b, const span_vertex_t* c,
                        float_gradients_t* out) {
    const float e1x = b->x - a->x, e1y = b->y - a->y;
    const float e2x = c->x - a->x, e2y = c->y - a->y;
    const float area2 = e1x * e2y - e2x * e1y;
    if (area2 > -1e-6f && area2 < 1e-6f) {
        return false;
    }
    const float inv = 1.0f / area2;
    const float va[4] = {a->z * FLOAT_DEPTH_ONE, a->r * FLOAT_COLOR_ONE, a->g * FLOAT_COLOR_ONE,
                         a->b * FLOAT_COLOR_ONE};
    const float vb[4] = {b->z * FLOAT_DEPTH_ONE, b->r * FLOAT_COLOR_ONE, b->g * FLOAT_COLOR_ONE,
                         b->b * FLOAT_COLOR_ONE};
    const float vc[4] = {c->z * FLOAT_DEPTH_ONE, c->r * FLOAT_COLOR_ONE, c->g * FLOAT_COLOR_ONE,
                         c->b * FLOAT_COLOR_ONE};
    for (int k = 0; k < 4; k++) {
        const float d1 = vb[k] - va[k], d2 = vc[k] - va[k];
        out->ddx[k] = (d1 * e2y - d2 * e1y) * inv;
        out->ddy[k] = (d2 * e1x - d1 * e2x) * inv;
        out->at[k] = va[k];
        out->dx[k] = float_to_step(out->ddx[k]);
    }
    out->x0 = a->x;
    out->y0 = a->y;
    return true;
}

/* A pixel centre just outside the triangle extrapolates past the vertex
 * range, and a wrapped channel would be a wrong-coloured pixel. The start
 * is clamped; only a span whose end would still leave the range pays for
 * a clamped end and a step recomputed from both. */
static void
float_fill_span(const span_target_t* target, const float_gradients_t* g, int y, int x_first, int x_last) {
    const float yc = (float)y + 0.5f - g->y0;
    const float xs = (float)x_first + 0.5f - g->x0;
    const int count = x_last - x_first;

    int32_t v[4], d[4];
    for (int k = 0; k < 4; k++) {
        const float start = clampf(g->at[k] + g->ddx[k] * xs + g->ddy[k] * yc, 0.0f, float_attribute_max[k]);
        const float end = start + g->ddx[k] * (float)count;
        v[k] = (int32_t)(uint32_t)start;
        d[k] = g->dx[k];
        if (count > 0 && (end < 0.0f || end > float_attribute_max[k])) {
            d[k] = float_to_step((clampf(end, 0.0f, float_attribute_max[k]) - start) / (float)count);
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
float_fill_flat_span(const span_target_t* target, int y, int x_first, int x_last, uint16_t zq, gfx_color_t color) {
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
#define FLOAT_FLAT_MAX_ROWS  2
#define FLOAT_FLAT_MAX_WIDTH 3.0f

static bool
float_is_tiny(const span_vertex_t* a, const span_vertex_t* b, const span_vertex_t* c, int rows) {
    if (rows > FLOAT_FLAT_MAX_ROWS) {
        return false;
    }
    const float lo = a->x < b->x ? (a->x < c->x ? a->x : c->x) : (b->x < c->x ? b->x : c->x);
    const float hi = a->x > b->x ? (a->x > c->x ? a->x : c->x) : (b->x > c->x ? b->x : c->x);
    return hi - lo <= FLOAT_FLAT_MAX_WIDTH;
}

static void
float_triangle(const span_target_t* target, const span_vertex_t* a, const span_vertex_t* b, const span_vertex_t* c) {
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

    const bool flat = float_is_tiny(a, b, c, y_end - y_first);
    float_gradients_t g;
    uint16_t flat_z = 0;
    gfx_color_t flat_color = 0;
    if (flat) {
        const float third = 1.0f / 3.0f;
        flat_z = (uint16_t)(clampf((a->z + b->z + c->z) * third, 0.0f, 1.0f) * 65535.0f);
        const float r = clampf((a->r + b->r + c->r) * third, 0.0f, 255.0f) * FLOAT_COLOR_ONE;
        const float gr = clampf((a->g + b->g + c->g) * third, 0.0f, 255.0f) * FLOAT_COLOR_ONE;
        const float bl = clampf((a->b + b->b + c->b) * third, 0.0f, 255.0f) * FLOAT_COLOR_ONE;
        flat_color = pack((int32_t)r, (int32_t)gr, (int32_t)bl);
    } else if (!float_compute_gradients(a, b, c, &g)) {
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
            float_fill_flat_span(target, y, x_first, x_last, flat_z, flat_color);
        } else {
            float_fill_span(target, &g, y, x_first, x_last);
        }
    }
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

    const float lo_x = a->x < b->x ? (a->x < c->x ? a->x : c->x) : (b->x < c->x ? b->x : c->x);
    const float hi_x = a->x > b->x ? (a->x > c->x ? a->x : c->x) : (b->x > c->x ? b->x : c->x);
    if (lo_x < -FIXED_COORD_LIMIT || hi_x > FIXED_COORD_LIMIT || v0->y < -FIXED_COORD_LIMIT
        || v2->y > FIXED_COORD_LIMIT) {
        float_triangle(target, a, b, c);
        return;
    }
    const bool flat = y_end - y_first <= FLAT_MAX_ROWS && hi_x - lo_x <= FLAT_MAX_WIDTH;

    const int y_anchor = fast_ceil(v0->y - 0.5f);
    int x_origin = fast_ceil(lo_x - 0.5f);
    x_origin = x_origin < 0 ? 0 : x_origin;
    gradients_t g;
    uint16_t flat_z = 0;
    gfx_color_t flat_color = 0;
    if (flat) {
        const float third = 1.0f / 3.0f;
        flat_z = (uint16_t)(clampf((a->z + b->z + c->z) * third, 0.0f, 1.0f) * 65535.0f);
        flat_color = pack((int32_t)(clampf((a->r + b->r + c->r) * third, 0.0f, 255.0f) * COLOR_SCALE),
                          (int32_t)(clampf((a->g + b->g + c->g) * third, 0.0f, 255.0f) * COLOR_SCALE),
                          (int32_t)(clampf((a->b + b->b + c->b) * third, 0.0f, 255.0f) * COLOR_SCALE));
    } else if (!compute_gradients(a, b, c, x_origin, y_anchor, &g)) {
        return;
    }
    if (span_raster_stop_after == 1) {
        return;
    }

    /* The long edge v0-v2 runs the whole height; the short side is v0-v1
     * above v1 and v1-v2 below it. */
    const int y_mid = fast_ceil(clampf(v1->y, top, bottom) - 0.5f);
    edge_t long_edge = edge_at(v0, v2, y_first);
    edge_t short_edge = y_first < y_mid ? edge_at(v0, v1, y_first) : edge_at(v1, v2, y_first);
    int32_t row[ATTRIBUTES];
    if (!flat) {
        for (int k = 0; k < ATTRIBUTES; k++) {
            row[k] = y_first == y_anchor ? g.base[k] : (int32_t)(g.base[k] + (int64_t)(y_first - y_anchor) * g.dy[k]);
        }
    }
    const int last_column = target->width - 1;

    for (int y = y_first; y < y_end; y++) {
        if (y == y_mid && y != y_first) {
            short_edge = edge_at(v1, v2, y);
        }
        const int32_t left = long_edge.x < short_edge.x ? long_edge.x : short_edge.x;
        const int32_t right = long_edge.x < short_edge.x ? short_edge.x : long_edge.x;
        int x_first = first_pixel(left);
        int x_last = first_pixel(right) - 1;
        x_first = x_first < 0 ? 0 : x_first;
        x_last = x_last > last_column ? last_column : x_last;

        if (x_first <= x_last && span_raster_stop_after != 2) {
            if (flat) {
                fill_flat_span(target, y, x_first, x_last, flat_z, flat_color);
            } else {
                fill_span(target, &g, row, y, x_first, x_last);
            }
        }
        long_edge.x += long_edge.step;
        short_edge.x += short_edge.step;
        if (!flat) {
            for (int k = 0; k < ATTRIBUTES; k++) {
                row[k] += g.dy[k];
            }
        }
    }
}
