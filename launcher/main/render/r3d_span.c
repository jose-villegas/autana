#include "render/r3d_span.h"
#include "render/r3d_span_internal.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "render/code_layout.h"

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

/* A triangle whose bounding box holds at most this many pixel centres a
 * side has each centre tested against its edges instead of walked: no sort,
 * no edge divides, no span setup. */
#define BOX_MAX_SIDE   4

/* Up to this many a side, it also takes one colour and one depth. */
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
        row[0] += f->g->dy[0];
        if (f->face == NULL) {
            for (int k = 1; k < ATTRIBUTES; k++) {
                row[k] += f->g->dy[k];
            }
        }
    }
}

/* An edge's first column at or right of it, row by row, exactly: ceil(n /
 * d) for an n growing a fixed step per row, held as quotient q and
 * remainder r in [0, d). Two triangles sharing the edge get the same column
 * on every row in any window, so one fills up to it and the other from it. */
typedef struct {
    int32_t q, r, qs, rs, d;
} edge_t;

static inline void
floor_div(int32_t n, int32_t d, int32_t* q, int32_t* r) {
    *q = n / d;
    *r = n - (*q * d);
    if (*r < 0) {
        *q -= 1;
        *r += d;
    }
}

/* Row y's centre line crosses the edge at x = top.x + (16 y + 8 - top.y)
 * dx / dy, and the first centre at or right of it is ceil((x - 8) / 16).
 * With every coordinate inside R3D_SPAN_RANGE the numerator stays under
 * 2^31 for any row of the edge. */
static edge_t
edge_at(const r3d_span_vertex_t* top, const r3d_span_vertex_t* bottom, int y) {
    const int32_t dx = bottom->x - top->x;
    const int32_t dy = bottom->y - top->y;
    const int32_t row_offset = (y << R3D_SUBPIXEL_SHIFT) + HALF_PIXEL - top->y;
    edge_t e = {0, 0, 0, 0, dy << R3D_SUBPIXEL_SHIFT};
    floor_div(((top->x - HALF_PIXEL) * dy) + (row_offset * dx) + e.d - 1, e.d, &e.q, &e.r);
    floor_div(dx, dy, &e.qs, &e.rs);
    e.rs <<= R3D_SUBPIXEL_SHIFT;
    return e;
}

static inline void
step_edge(edge_t* e) {
    e->q += e->qs;
    e->r += e->rs;
    if (e->r >= e->d) {
        e->r -= e->d;
        e->q++;
    }
}

static RENDER_ENTRY_OFFSET(6) void
walk_rows(const r3d_span_target_t* target, const fill_t* f, int32_t row[ATTRIBUTES], int y0, int y1, edge_t* left,
          edge_t* right) {
    for (int y = y0; y < y1; y++) {
        fill_row(target, f, row, y, left->q, right->q - 1);
        step_edge(left);
        step_edge(right);
        step_row(f, row);
    }
}

typedef struct {
    const r3d_span_vertex_t *v0, *v1, *v2; /* by y */
    int y_first, split, y_end;
    bool long_on_left;
} walk_t;

/* The long edge v0-v2 runs the whole height on one side; the short side is
 * v0-v1 above the split and v1-v2 below it. */
static void
walk(const r3d_span_target_t* target, const fill_t* f, int32_t row[ATTRIBUTES], const walk_t* w) {
    edge_t long_edge = edge_at(w->v0, w->v2, w->y_first);
    if (w->y_first < w->split) {
        edge_t upper = edge_at(w->v0, w->v1, w->y_first);
        walk_rows(target, f, row, w->y_first, w->split, w->long_on_left ? &long_edge : &upper,
                  w->long_on_left ? &upper : &long_edge);
    }
    if (w->split < w->y_end) {
        edge_t lower = edge_at(w->v1, w->v2, w->split);
        walk_rows(target, f, row, w->split, w->y_end, w->long_on_left ? &long_edge : &lower,
                  w->long_on_left ? &lower : &long_edge);
    }
}

/* Returns true when the sort swapped an odd number of times, which turns
 * the triangle's winding over. */
static inline bool
sort_by_y(const r3d_span_vertex_t** v0, const r3d_span_vertex_t** v1, const r3d_span_vertex_t** v2) {
    const r3d_span_vertex_t* t;
    bool odd = false;
    if ((*v1)->y < (*v0)->y) {
        t = *v0, *v0 = *v1, *v1 = t, odd = !odd;
    }
    if ((*v2)->y < (*v1)->y) {
        t = *v1, *v1 = *v2, *v2 = t, odd = !odd;
    }
    if ((*v1)->y < (*v0)->y) {
        t = *v0, *v0 = *v1, *v1 = t, odd = !odd;
    }
    return odd;
}

static inline int
clampi(int v, int lo, int hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

static inline void
set_flat(fill_t* f, const r3d_span_vertex_t* a, const r3d_span_vertex_t* b, const r3d_span_vertex_t* c) {
    const float third = 1.0F / 3.0F;
    f->flat_z = (uint16_t)(clampf((a->z + b->z + c->z) * third, 0.0F, 1.0F) * 65535.0F);
    f->flat_color = r3d_span_pack((int32_t)(clampf((a->r + b->r + c->r) * third, 0.0F, 255.0F) * COLOR_SCALE),
                                  (int32_t)(clampf((a->g + b->g + c->g) * third, 0.0F, 255.0F) * COLOR_SCALE),
                                  (int32_t)(clampf((a->b + b->b + c->b) * third, 0.0F, 255.0F) * COLOR_SCALE));
}

static inline int32_t
first_row_value(const gradients_t* g, int k, int y_first, int y_anchor) {
    return (int32_t)(g->base[k] + ((int64_t)(y_first - y_anchor) * g->dy[k]));
}

/* Edge a-b of a triangle winding positive, where (b - a) x (p - a) is above
 * zero inside; a centre exactly on it belongs to it only on a top or left
 * edge, which is the walk's rule in the same integers. */
typedef struct {
    int32_t ax, ay, dx, dy, bias;
} small_edge_t;

static inline small_edge_t
small_edge(const r3d_span_vertex_t* a, const r3d_span_vertex_t* b) {
    const int32_t dx = b->x - a->x;
    const int32_t dy = b->y - a->y;
    const bool top_left = dy < 0 || (dy == 0 && dx > 0);
    return (small_edge_t){a->x, a->y, dx, dy, top_left ? 0 : 1};
}

/* Negative when the centre is outside. */
static inline int32_t
small_side(const small_edge_t* e, int32_t x, int32_t y) {
    return (e->dx * (y - e->ay)) - (e->dy * (x - e->ax)) - e->bias;
}

/* An edge's value at the box's first centre, and what a column and a row
 * add to it: the same integers small_side() gives at each centre. */
typedef struct {
    int32_t w, across, down;
} box_edge_t;

typedef struct {
    box_edge_t e[3];
} box_edges_t;

static inline box_edges_t
box_edges(const r3d_span_vertex_t* a, const r3d_span_vertex_t* b, const r3d_span_vertex_t* c, bool positive,
          r3d_span_box_t box) {
    const r3d_span_vertex_t* p = positive ? b : c;
    const r3d_span_vertex_t* q = positive ? c : b;
    const small_edge_t s[3] = {small_edge(a, p), small_edge(p, q), small_edge(q, a)};
    const int32_t cx = (box.x0 << R3D_SUBPIXEL_SHIFT) + HALF_PIXEL;
    const int32_t cy = (box.y0 << R3D_SUBPIXEL_SHIFT) + HALF_PIXEL;
    box_edges_t out;
    for (int k = 0; k < 3; k++) {
        out.e[k] = (box_edge_t){small_side(&s[k], cx, cy), -s[k].dy * R3D_SUBPIXEL, s[k].dx * R3D_SUBPIXEL};
    }
    return out;
}

static void
fill_box_flat(const r3d_span_target_t* target, box_edges_t edges, r3d_span_box_t box, uint16_t zq, uint16_t color) {
    for (int y = box.y0; y < box.y1; y++) {
        const int row = (y - target->row0) * target->width;
        uint16_t* depth = target->depth + row;
        uint16_t* out = target->color + row;
        int32_t w0 = edges.e[0].w;
        int32_t w1 = edges.e[1].w;
        int32_t w2 = edges.e[2].w;
        for (int x = box.x0; x < box.x1; x++) {
            if ((w0 | w1 | w2) >= 0 && zq > depth[x]) {
                depth[x] = zq;
                out[x] = color;
            }
            w0 += edges.e[0].across;
            w1 += edges.e[1].across;
            w2 += edges.e[2].across;
        }
        for (int k = 0; k < 3; k++) {
            edges.e[k].w += edges.e[k].down;
        }
    }
}

static inline void
box_row_solid(uint16_t* depth, uint16_t* out, box_edges_t edges, int x0, int x1, int32_t z, int32_t dz,
              uint16_t color) {
    int32_t w0 = edges.e[0].w;
    int32_t w1 = edges.e[1].w;
    int32_t w2 = edges.e[2].w;
    for (int x = x0; x < x1; x++) {
        const uint16_t zq = (uint16_t)(z >> 8);
        if ((w0 | w1 | w2) >= 0 && zq > depth[x]) {
            depth[x] = zq;
            out[x] = color;
        }
        w0 += edges.e[0].across;
        w1 += edges.e[1].across;
        w2 += edges.e[2].across;
        z += dz;
    }
}

static inline void
box_row_smooth(uint16_t* depth, uint16_t* out, box_edges_t edges, int x0, int x1, const int32_t row[ATTRIBUTES],
               const gradients_t* g) {
    int32_t w0 = edges.e[0].w;
    int32_t w1 = edges.e[1].w;
    int32_t w2 = edges.e[2].w;
    int32_t z = row[0];
    int32_t r = row[1];
    int32_t gg = row[2];
    int32_t b = row[3];
    for (int x = x0; x < x1; x++) {
        const uint16_t zq = (uint16_t)(z >> 8);
        if ((w0 | w1 | w2) >= 0 && zq > depth[x]) {
            depth[x] = zq;
            out[x] = r3d_span_pack(r, gg, b);
        }
        w0 += edges.e[0].across;
        w1 += edges.e[1].across;
        w2 += edges.e[2].across;
        z += g->dx[0];
        r += g->dx[1];
        gg += g->dx[2];
        b += g->dx[3];
    }
}

/* The planes at each covered centre are the values the walk would reach
 * there: it never clamps a plane that stays in range across the box. */
static void
fill_box_planes(const r3d_span_target_t* target, const fill_t* f, box_edges_t edges, r3d_span_box_t box,
                int32_t row[ATTRIBUTES]) {
    for (int y = box.y0; y < box.y1; y++) {
        const int offset = (y - target->row0) * target->width;
        if (f->face != NULL) {
            box_row_solid(target->depth + offset, target->color + offset, edges, box.x0, box.x1, row[0], f->g->dx[0],
                          f->flat_color);
        } else {
            box_row_smooth(target->depth + offset, target->color + offset, edges, box.x0, box.x1, row, f->g);
        }
        for (int k = 0; k < 3; k++) {
            edges.e[k].w += edges.e[k].down;
        }
        step_row(f, row);
    }
}

/* Out of line: inlined into the triangle's entry, the box fills crowd the
 * registers of the walk every larger triangle takes. */
static __attribute__((noinline)) void
fill_box(const r3d_span_target_t* target, const fill_t* f, const r3d_span_vertex_t* const v[3], bool positive,
         r3d_span_box_t box, int32_t row[ATTRIBUTES]) {
    const box_edges_t edges = box_edges(v[0], v[1], v[2], positive, box);
    if (f->flat) {
        fill_box_flat(target, edges, box, f->flat_z, f->flat_color);
    } else {
        fill_box_planes(target, f, edges, box, row);
    }
}

static void
fill_small(const r3d_span_target_t* target, const r3d_span_vertex_t* const v[3], bool positive, r3d_span_box_t box,
           const uint16_t* face) {
    fill_t f = {true, face, 0, 0, NULL};
    set_flat(&f, v[0], v[1], v[2]);
    if (face != NULL) {
        f.flat_color = *face;
    }
    if (r3d_span_stop_after != 0) {
        return;
    }
    fill_box(target, &f, v, positive, box, NULL);
}

/* A span runs from its clamped start to its end, both values of the plane
 * at centres in `box`, so the largest corner bounds it; a start clamped up
 * from below zero lifts the span by at most the lowest corner's shortfall.
 * The corners lie past where the walk steps, so the sums need 64 bits. */
int32_t
r3d_span_plane_bound(int32_t top, int32_t dx, int32_t dy, r3d_span_box_t box) {
    const int64_t across = (int64_t)dx * (box.x1 - 1 - box.x0);
    const int64_t bottom = top + ((int64_t)dy * (box.y1 - 1 - box.y0));
    const int64_t low = (top < bottom ? top : bottom) + (across < 0 ? across : 0);
    const int64_t high = (top > bottom ? top : bottom) + (across > 0 ? across : 0);
    const int64_t lifted = high - (low < 0 ? low : 0);
    return (int32_t)((lifted < value_max[0] ? lifted : value_max[0]) >> 8);
}

bool
r3d_span_plane_in_range(int32_t top, int32_t dx, int32_t dy, int32_t max, r3d_span_box_t box) {
    const int64_t across = (int64_t)dx * (box.x1 - 1 - box.x0);
    const int64_t bottom = top + ((int64_t)dy * (box.y1 - 1 - box.y0));
    const int64_t low = (top < bottom ? top : bottom) + (across < 0 ? across : 0);
    const int64_t high = (top > bottom ? top : bottom) + (across > 0 ? across : 0);
    return low >= 0 && high <= max;
}

RENDER_ENTRY_OFFSET(8) bool
r3d_span_hidden(const r3d_span_target_t* target, int32_t bound, r3d_span_box_t box) {
    for (int y = box.y0; y < box.y1; y++) {
        const uint16_t* depth = target->depth + ((y - target->row0) * target->width);
        for (int x = box.x0; x < box.x1; x++) {
            if (depth[x] < bound) {
                return false;
            }
        }
    }
    return true;
}

/* The colour planes, unless a face colour stands in for them, and whether
 * every plane stays in range across the box so spans need no clamp. */
static void
set_up_planes(const fill_t* f, const r3d_span_vertex_t* const v[3], const plane_t* p, r3d_span_box_t box, int y_anchor,
              gradients_t* g, int32_t row[ATTRIBUTES]) {
    g->in_range = r3d_span_plane_in_range(row[0], g->dx[0], g->dy[0], value_max[0], box);
    if (f->face != NULL) {
        return;
    }
    colour_gradients(p, v[0], v[1], v[2], g);
    for (int k = 1; k < ATTRIBUTES; k++) {
        row[k] = first_row_value(g, k, box.y0, y_anchor);
        g->in_range = g->in_range && r3d_span_plane_in_range(row[k], g->dx[k], g->dy[k], value_max[k], box);
    }
}

/* The depth plane first, then the colour planes only for a triangle not
 * already hidden; false when it would write nothing. */
static bool
set_up_fill(const r3d_span_target_t* target, const r3d_span_vertex_t* const v[3], r3d_span_box_t box, int y_anchor,
            fill_t* f, gradients_t* g, int32_t row[ATTRIBUTES]) {
    plane_t p;
    g->x_origin = box.x0;
    if (f->flat) {
        set_flat(f, v[0], v[1], v[2]);
    } else if (plane_of(v[0], v[1], v[2], box.x0, y_anchor, &p)) {
        gradient(&p, v[0]->z * DEPTH_SCALE, v[1]->z * DEPTH_SCALE, v[2]->z * DEPTH_SCALE, 0, g);
        row[0] = first_row_value(g, 0, box.y0, y_anchor);
    } else {
        return false;
    }
    if (f->face != NULL) {
        f->flat_color = *f->face;
    }
    const int32_t bound = f->flat ? f->flat_z : r3d_span_plane_bound(row[0], g->dx[0], g->dy[0], box);
    if (r3d_span_hidden(target, bound, box)) {
        return false;
    }
    if (!f->flat) {
        set_up_planes(f, v, &p, box, y_anchor, g, row);
    }
    return true;
}

static RENDER_ENTRY_OFFSET(14) void
r3d_span_triangle_impl(const r3d_span_target_t* target, const r3d_span_vertex_t* a, const r3d_span_vertex_t* b,
                       const r3d_span_vertex_t* c, const uint16_t* face) {
    const r3d_span_extent_t extent = r3d_span_extent(a, b, c);
    const r3d_span_box_t box = r3d_span_clip(target, extent.centres);
    if (box.y0 >= box.y1 || box.x0 >= box.x1) {
        return; /* no pixel centre inside this window */
    }
    const int32_t area2 = r3d_span_area2(a, b, c);
    const r3d_span_vertex_t* const v[3] = {a, b, c};
    if (area2 == 0) {
        return;
    }
    const int columns = extent.centres.x1 - extent.centres.x0;
    const int rows = extent.centres.y1 - extent.centres.y0;
    if (columns <= SMALL_MAX_SIDE && rows <= SMALL_MAX_SIDE) {
        fill_small(target, v, area2 > 0, box, face);
        return;
    }

    fill_t f = {extent.flat, face, 0, 0, NULL};
    /* Attributes anchor at the triangle's first row, or at screen row 0 for
     * one starting above the screen: never at a window's own edge. */
    const int y_anchor = clampi(extent.centres.y0, -1, target->row1 + 1);
    gradients_t g = {0};
    int32_t row[ATTRIBUTES] = {0};
    f.g = &g;
    if (!set_up_fill(target, v, box, y_anchor, &f, &g, row)) {
        return;
    }
    if (columns <= BOX_MAX_SIDE && rows <= BOX_MAX_SIDE && (f.flat || g.in_range)) {
        if (r3d_span_stop_after != 0) {
            return;
        }
        fill_box(target, &f, v, area2 > 0, box, row);
        return;
    }
    if (r3d_span_stop_after == 1) {
        return;
    }
    const r3d_span_vertex_t* v0 = a;
    const r3d_span_vertex_t* v1 = b;
    const r3d_span_vertex_t* v2 = c;
    const bool odd = sort_by_y(&v0, &v1, &v2);
    /* v1 lies right of the long edge v0-v2 when v0-v1-v2 winds positive. */
    const walk_t w = {
        v0, v1, v2, box.y0, clampi(r3d_span_first_centre(v1->y), box.y0, box.y1), box.y1, (area2 > 0) != odd};
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
