#include "render/r3d_span.h"
#include "render/r3d_span_internal.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#pragma GCC diagnostic error "-Wdouble-promotion"

/* Attributes run in fixed point: depth as 16.8, colour channels as 8.8, whose
 * steepest real step (255 levels in one pixel) is far below the clamp. Only a
 * sliver's depth step can reach 2^22; with that bound and at most a screen of
 * steps from the triangle's corner, every sum stays inside int32. */
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
    bool in_range; /* every plane stays inside its range across the whole box */
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

static inline float
pixels_of(int32_t subpixels) {
    return (float)subpixels * (1.0F / (float)R3D_SUBPIXEL);
}

/* What every attribute's plane shares: the edges from corner a, the
 * reciprocal of twice the area, and the origin pixel's centre from a. */
typedef struct {
    float e1x, e1y, e2x, e2y, inv, ox, oy;
} plane_t;

static bool
plane_of(const r3d_span_vertex_t* a, const r3d_span_vertex_t* b, const r3d_span_vertex_t* c, int x_origin, int y_anchor,
         plane_t* p) {
    const float ax = pixels_of(a->x);
    const float ay = pixels_of(a->y);
    p->e1x = pixels_of(b->x) - ax;
    p->e1y = pixels_of(b->y) - ay;
    p->e2x = pixels_of(c->x) - ax;
    p->e2y = pixels_of(c->y) - ay;
    const float area2 = (p->e1x * p->e2y) - (p->e2x * p->e1y);
    if (area2 > -1e-6F && area2 < 1e-6F) {
        return false;
    }
    p->inv = 1.0F / area2;
    p->ox = (float)x_origin + 0.5F - ax;
    p->oy = (float)y_anchor + 0.5F - ay;
    return true;
}

static void
gradient(const plane_t* p, float va, float vb, float vc, int k, gradients_t* out) {
    const float d1 = vb - va;
    const float d2 = vc - va;
    const float ddx = (d1 * p->e2y - d2 * p->e1y) * p->inv;
    const float ddy = (d2 * p->e1x - d1 * p->e2x) * p->inv;
    out->dx[k] = to_step(ddx);
    out->dy[k] = to_step(ddy);
    out->base[k] = (int32_t)clampf(va + (ddx * p->ox) + (ddy * p->oy), -2.0e9F, 2.0e9F);
}

/* The colour planes, left until the depth plane has shown the triangle is
 * not hidden. */
static void
colour_gradients(const plane_t* p, const r3d_span_vertex_t* a, const r3d_span_vertex_t* b, const r3d_span_vertex_t* c,
                 gradients_t* out) {
    gradient(p, a->r * COLOR_SCALE, b->r * COLOR_SCALE, c->r * COLOR_SCALE, 1, out);
    gradient(p, a->g * COLOR_SCALE, b->g * COLOR_SCALE, c->g * COLOR_SCALE, 2, out);
    gradient(p, a->b * COLOR_SCALE, b->b * COLOR_SCALE, c->b * COLOR_SCALE, 3, out);
}

/* A pixel centre just outside the triangle extrapolates past the vertex
 * range, and a wrapped channel would be a wrong-coloured pixel. Each span's
 * start is clamped; only a span whose end would still leave the range pays
 * for a step recomputed from both clamped ends. */
static inline void
span_step(const gradients_t* g, const int32_t row[ATTRIBUTES], int k, int offset, int count, int32_t* value,
          int32_t* step) {
    *step = g->dx[k];
    if (g->in_range) {
        *value = row[k] + (g->dx[k] * offset);
        return;
    }
    const int32_t start = clamp_value(row[k] + (g->dx[k] * offset), value_max[k]);
    const int32_t end = start + (g->dx[k] * count);
    *value = start;
    if (count > 0 && (end < 0 || end > value_max[k])) {
        *step = (clamp_value(end, value_max[k]) - start) / count;
    }
}

static void
fill_span(const r3d_span_target_t* target, const gradients_t* g, const int32_t row[ATTRIBUTES], int y, int x_first,
          int x_last) {
    const int32_t offset = x_first - g->x_origin;
    const int count = x_last - x_first;
    int32_t v[ATTRIBUTES];
    int32_t d[ATTRIBUTES];
    for (int k = 0; k < ATTRIBUTES; k++) {
        span_step(g, row, k, offset, count, &v[k], &d[k]);
    }
    if (r3d_span_stop_after == 3) {
        return;
    }

    const int row_offset = (y - target->row0) * target->width;
    uint16_t* depth = target->depth + row_offset;
    uint16_t* color = target->color + row_offset;
    int32_t z = v[0];
    int32_t r = v[1];
    int32_t gg = v[2];
    int32_t b = v[3];
    for (int x = x_first; x <= x_last; x++) {
        const uint16_t zq = (uint16_t)(z >> 8);
        if (zq > depth[x]) {
            depth[x] = zq;
            color[x] = r3d_span_pack(r, gg, b);
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

/* A face's own colour with the depth plane walked as usual. */
static void
fill_solid_span(const r3d_span_target_t* target, const gradients_t* g, const int32_t row[ATTRIBUTES], int y,
                int x_first, int x_last, uint16_t color) {
    int32_t z;
    int32_t dz;
    span_step(g, row, 0, x_first - g->x_origin, x_last - x_first, &z, &dz);
    const int row_offset = (y - target->row0) * target->width;
    uint16_t* depth = target->depth + row_offset;
    uint16_t* out = target->color + row_offset;
    for (int x = x_first; x <= x_last; x++) {
        const uint16_t zq = (uint16_t)(z >> 8);
        if (zq > depth[x]) {
            depth[x] = zq;
            out[x] = color;
        }
        z += dz;
    }
}

/* Small enough that a gradient across it is invisible: one colour, one
 * depth. Its coverage is still decided by the same edges. */
#define FLAT_MAX_ROWS  2
#define FLAT_MAX_WIDTH (3 * R3D_SUBPIXEL)

/* A triangle whose bounding box holds at most this many pixel centres a
 * side has each centre tested against its edges instead of walked. */
#define SMALL_MAX_SIDE 2

#define HALF_PIXEL     (R3D_SUBPIXEL / 2)

typedef struct {
    bool flat;
    const uint16_t* face; /* the colour every pixel takes, or NULL for the vertices' */
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
    } else if (f->face != NULL) {
        fill_solid_span(target, f->g, row, y, x_first, x_last, f->flat_color);
    } else {
        fill_span(target, f->g, row, y, x_first, x_last);
    }
}

static inline void
step_row(const fill_t* f, int32_t row[ATTRIBUTES]) {
    if (!f->flat) {
        set_up_planes(f, v, &p, box, y_anchor, g, row);
    }
    return true;
}

static void
r3d_span_triangle_impl(const r3d_span_target_t* target, const r3d_span_vertex_t* a, const r3d_span_vertex_t* b,
                       const r3d_span_vertex_t* c, const uint16_t* face) {
    const r3d_span_vertex_t* v0 = a;
    const r3d_span_vertex_t* v1 = b;
    const r3d_span_vertex_t* v2 = c;
    const bool odd = sort_by_y(&v0, &v1, &v2);

    /* The whole triangle's rows, not the window's, decide its path, so any
     * window of rows draws exactly those rows of the whole. */
    const int rows = r3d_span_first_centre(v2->y) - r3d_span_first_centre(v0->y);
    const int y_first = clampi(r3d_span_first_centre(v0->y), target->row0, target->row1);
    const int y_end = clampi(r3d_span_first_centre(v2->y), target->row0, target->row1);
    const int32_t lo_x = r3d_span_min3(a->x, b->x, c->x);
    const int32_t hi_x = r3d_span_max3(a->x, b->x, c->x);
    const int x_first = r3d_span_first_centre(lo_x);
    const int x_end = r3d_span_first_centre(hi_x);
    const r3d_span_box_t box = {x_first < 0 ? 0 : x_first, x_end > target->width ? target->width : x_end, y_first,
                                y_end};
    if (box.y0 >= box.y1 || box.x0 >= box.x1) {
        return; /* no pixel centre inside this window */
    }
    /* Twice the signed area of a-b-c; each product is under 2^30. */
    const int32_t area2 = ((b->x - a->x) * (c->y - a->y)) - ((b->y - a->y) * (c->x - a->x));
    if (area2 == 0) {
        return;
    }
    if (x_end - x_first <= SMALL_MAX_SIDE && rows <= SMALL_MAX_SIDE) {
        fill_small(target, a, b, c, area2 > 0, box, face);
        return;
    }

    fill_t f = {rows <= FLAT_MAX_ROWS && hi_x - lo_x <= FLAT_MAX_WIDTH, face, 0, 0, NULL};
    /* Attributes anchor at the triangle's first row, or at screen row 0 for
     * one starting above the screen: never at a window's own edge. */
    const int y_anchor = clampi(r3d_span_first_centre(v0->y), -1, target->row1 + 1);
    gradients_t g = {0};
    int32_t row[ATTRIBUTES] = {0};
    f.g = &g;
    if (!set_up_fill(target, (const r3d_span_vertex_t* const[3]){a, b, c}, box, y_anchor, &f, &g, row)) {
        return;
    }
    if (r3d_span_stop_after == 1) {
        return;
    }
    /* v1 lies right of the long edge v0-v2 when v0-v1-v2 winds positive. */
    const walk_t w = {
        v0, v1, v2, y_first, clampi(r3d_span_first_centre(v1->y), y_first, y_end), y_end, (area2 > 0) != odd};
    walk(target, &f, row, &w);
}

void
r3d_span_triangle(const r3d_span_target_t* target, const r3d_span_vertex_t* a, const r3d_span_vertex_t* b,
                  const r3d_span_vertex_t* c) {
    r3d_span_triangle_impl(target, a, b, c, NULL);
}

void
r3d_span_triangle_solid(const r3d_span_target_t* target, const r3d_span_vertex_t* a, const r3d_span_vertex_t* b,
                        const r3d_span_vertex_t* c, uint16_t color) {
    r3d_span_triangle_impl(target, a, b, c, &color);
}
