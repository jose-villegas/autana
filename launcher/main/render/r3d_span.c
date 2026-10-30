#include "render/r3d_span.h"
#include "render/r3d_span_internal.h"

#include <limits.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "core/tune.h"
#include "image/pixel/bayer.h"
#include "image/pixel/rgb565.h"
#include "math/scalar/fixed.h"
#include "math/scalar/mathf.h"
#include "math/scalar/mathi.h"
#include "profile/frame_laps.h"
#include "render/code_layout.h"

/* Attributes run in fixed point: depth as 16.8, colour channels as 8.8, whose
 * steepest real step (255 levels in one pixel) is far below the clamp. Only a
 * sliver's depth step can reach 2^22; with that bound and at most a screen of
 * steps from the triangle's corner, every sum stays inside int32. A vertex
 * past the screen's edge may store more than DEPTH_MAX, as depth is scaled to
 * the nearest thing on screen; the plane's base, taken on screen, comes back
 * in range at the cost of a code or two of float rounding. */
#define ATTRIBUTES  4
#define DEPTH_MAX   ((int32_t)R3D_DEPTH_NEAREST << R3D_DEPTH_SHIFT)
#define DEPTH_SCALE ((float)DEPTH_MAX)
#define STEP_MAX    4194304.0f

_Static_assert(((int64_t)R3D_DEPTH_NEAREST << R3D_DEPTH_SHIFT) <= INT32_MAX, "16.8 depth fits int32");

static const int32_t value_max[ATTRIBUTES] = {DEPTH_MAX, 65280, 65280, 65280};

#define SMALL_MAX_SIDE         R3D_SPAN_SMALL_MAX_SIDE
#define SMALL_SPREAD_OFF       ((int)RGB565_G6_MAX + 1)
#define SMALL_DITHER_SIDE      (sizeof bayer4x4 / sizeof bayer4x4[0])
#define SMALL_DITHER_MASK      (SMALL_DITHER_SIDE - 1)
#define SMALL_DITHER_SHIFT     (1 + 2 * __builtin_ctz((unsigned)SMALL_DITHER_SIDE))
#define SMALL_GRAIN_PAIR       2
#define SMALL_GRAIN_GREEN_STEP 2
#define SMALL_GRAIN_RB_MAX     ((int)RGB565_G6_MAX >> 1)
#define SMALL_GRAIN_MIDPOINT   (((2 * SMALL_GRAIN_GREEN_STEP * SMALL_GRAIN_RB_MAX) + (int)RGB565_G6_MAX + 1) / 2)
#define SMALL_FILL_WALK        (-1)
#define TRIANGLE_VERTICES      3

TUNE_OWNER(r3d_span);
TUNE_HOT(r3d_span, flat_max_rows, R3D_SPAN_FLAT_MAX_ROWS, 0, 8);
TUNE_HOT(r3d_span, flat_max_width, R3D_SPAN_FLAT_MAX_WIDTH, 0, 8 * R3D_SUBPIXEL);

int r3d_span_stop_after;

typedef struct {
    int32_t base[ATTRIBUTES]; /* plane origins during setup, then attributes carried at the left edge */
    int32_t dx[ATTRIBUTES];
    int32_t dy[ATTRIBUTES];
    int x_origin;
    bool in_range; /* every plane stays inside its range across the whole box */
} gradients_t;

static inline int32_t
to_step(float step) {
    return (int32_t)mathf_clamp(step, -STEP_MAX, STEP_MAX);
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

static inline __attribute__((always_inline)) bool
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
    p->inv = mathf_recip(area2);
    p->ox = (float)x_origin + 0.5F - ax;
    p->oy = (float)y_anchor + 0.5F - ay;
    return true;
}

static inline __attribute__((always_inline)) void
gradient(const plane_t* p, float va, float vb, float vc, int k, gradients_t* out) {
    const float d1 = vb - va;
    const float d2 = vc - va;
    const float ddx = (d1 * p->e2y - d2 * p->e1y) * p->inv;
    const float ddy = (d2 * p->e1x - d1 * p->e2x) * p->inv;
    out->dx[k] = to_step(ddx);
    out->dy[k] = to_step(ddy);
    out->base[k] = (int32_t)mathf_clamp(va + (ddx * p->ox) + (ddy * p->oy), -2.0e9F, 2.0e9F);
}

/* The colour planes, left until the depth plane has shown the triangle is
 * not hidden. */
static void
colour_gradients(const plane_t* p, const r3d_span_vertex_t* a, const r3d_span_vertex_t* b, const r3d_span_vertex_t* c,
                 gradients_t* out) {
    gradient(p, a->r * R3D_SPAN_COLOR_SCALE, b->r * R3D_SPAN_COLOR_SCALE, c->r * R3D_SPAN_COLOR_SCALE, 1, out);
    gradient(p, a->g * R3D_SPAN_COLOR_SCALE, b->g * R3D_SPAN_COLOR_SCALE, c->g * R3D_SPAN_COLOR_SCALE, 2, out);
    gradient(p, a->b * R3D_SPAN_COLOR_SCALE, b->b * R3D_SPAN_COLOR_SCALE, c->b * R3D_SPAN_COLOR_SCALE, 3, out);
}

/* A pixel centre just outside the triangle extrapolates past the vertex
 * range, and a wrapped channel would be a wrong-coloured pixel. Each span's
 * start is clamped; only a span whose end would still leave the range pays
 * for a step recomputed from both clamped ends. */
static inline void
clamp_span(int k, int count, int32_t* value, int32_t* step) {
    const int32_t start = mathi_clamp(*value, 0, value_max[k]);
    const int32_t end = start + (*step * count);
    *value = start;
    if (count > 0 && (end < 0 || end > value_max[k])) {
        *step = (mathi_clamp(end, 0, value_max[k]) - start) / count;
    }
}

static inline void
span_start(const gradients_t* g, uint32_t carried, int k, int skip, int count, int32_t* value, int32_t* step) {
    *step = g->dx[k];
    *value = (int32_t)carried;
    if (skip > 0) {
        *value = (int32_t)(carried + ((uint32_t)*step * (uint32_t)skip));
    }
    if (!g->in_range) {
        clamp_span(k, count, value, step);
    }
}

#define HALF_PIXEL (R3D_SUBPIXEL / 2)

typedef enum { FILL_FLAT, FILL_FACE, FILL_GOURAUD } shading_t;

typedef struct {
    shading_t shading;
    uint16_t flat_z;
    uint16_t flat_color;
    gradients_t* g;
    int last_column;
} fill_t;

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

static inline __attribute__((always_inline)) bool
step_edge(edge_t* e) {
    e->q += e->qs;
    e->r += e->rs;
    if (e->r >= e->d) {
        e->r -= e->d;
        e->q++;
        return true;
    }
    return false;
}

typedef struct {
    uint16_t* depth;
    uint16_t* color;
    int32_t centre;
} pixel_row_t;

static inline __attribute__((always_inline)) pixel_row_t
pixel_row(const r3d_span_target_t* target, int y) {
    const gfx_target_t* rows = &target->rows;
    const size_t offset = (size_t)(y - rows->first_row) * rows->attachment[GFX_ATTACHMENT_COLOR].pitch;
    return (pixel_row_t){(uint16_t*)((char*)rows->attachment[GFX_ATTACHMENT_DEPTH].pixels + offset),
                         (uint16_t*)((char*)rows->attachment[GFX_ATTACHMENT_COLOR].pixels + offset),
                         (y << R3D_SUBPIXEL_SHIFT) + HALF_PIXEL};
}

typedef struct {
    int first, last;
    uint16_t *depth, *color;
} row_span_t;

static inline __attribute__((always_inline)) bool
clip_row(const r3d_span_target_t* target, int last_column, size_t offset, const edge_t* left, const edge_t* right,
         row_span_t* span) {
    span->first = left->q < 0 ? 0 : left->q;
    span->last = right->q - 1 > last_column ? last_column : right->q - 1;
    if (span->first > span->last || r3d_span_stop_after == 3) {
        return false;
    }
    span->depth = (uint16_t*)((char*)target->rows.attachment[GFX_ATTACHMENT_DEPTH].pixels + offset) + span->first;
    span->color = (uint16_t*)((char*)target->rows.attachment[GFX_ATTACHMENT_COLOR].pixels + offset) + span->first;
    return true;
}

/* Separate register windows keep the row walk's edges out of the pixel loops. */
static RENDER_IRAM_ATTR __attribute__((noinline)) void
fill_flat(uint16_t* depth, uint16_t* color, int count, uint16_t z, uint16_t face) {
    for (int i = 0; i < count; i++) {
        if (z > depth[i]) {
            depth[i] = z;
            color[i] = face;
        }
    }
}

static RENDER_IRAM_ATTR __attribute__((noinline)) void
fill_solid(uint16_t* depth, uint16_t* color, int count, int32_t z, int32_t dz, uint16_t face) {
    for (int i = 0; i < count; i++) {
        const uint16_t zq = (uint16_t)(z >> R3D_DEPTH_SHIFT);
        if (zq > depth[i]) {
            depth[i] = zq;
            color[i] = face;
        }
        z += dz;
    }
}

#if defined(__XTENSA__) && __has_attribute(optimize)
#define FILL_GOURAUD_ATTR __attribute__((optimize("no-schedule-insns")))
#else
#define FILL_GOURAUD_ATTR
#endif

/* Packing after the depth test bounds live temporaries to the register window.
 * On Xtensa, disabling scheduling saves 2.267 million instructions per pose
 * at 368x448 under QEMU --icount and keeps the filler spill-free. */
static RENDER_IRAM_ATTR __attribute__((noinline)) FILL_GOURAUD_ATTR void
fill_gouraud(uint16_t* depth, uint16_t* color, int count, const uint32_t value[ATTRIBUTES],
             const int32_t step[ATTRIBUTES]) {
    int32_t z = (int32_t)value[0];
    int32_t r = (int32_t)value[1];
    int32_t g = (int32_t)value[2];
    int32_t b = (int32_t)value[3];
    const int32_t dz = step[0];
    const int32_t dr = step[1];
    const int32_t dg = step[2];
    const int32_t db = step[3];
    for (int i = 0; i < count; i++) {
        const uint16_t zq = (uint16_t)(z >> R3D_DEPTH_SHIFT);
        if (zq > depth[i]) {
            depth[i] = zq;
            color[i] = r3d_span_pack(r, g, b);
        }
        z += dz;
        r += dr;
        g += dg;
        b += db;
    }
}

static inline __attribute__((always_inline)) void
fill_clipped_gouraud(const gradients_t* g, const uint32_t row[ATTRIBUTES], int column, const row_span_t* span) {
    uint32_t value[ATTRIBUTES];
    int32_t step[ATTRIBUTES];
    for (int k = 0; k < ATTRIBUTES; k++) {
        int32_t start;
        span_start(g, row[k], k, column < 0 ? -column : 0, span->last - span->first, &start, &step[k]);
        value[k] = (uint32_t)start;
    }
    fill_gouraud(span->depth, span->color, span->last - span->first + 1, value, step);
}

static void write_span(const r3d_span_target_t* target, int y, int x_first, int x_last, int32_t z, int32_t dz);

static inline __attribute__((always_inline)) void
flat_rows(const r3d_span_target_t* target, const fill_t* f, int y0, int y1, edge_t* left, edge_t* right, bool writing) {
    const size_t pitch = target->rows.attachment[GFX_ATTACHMENT_COLOR].pitch;
    size_t offset = (size_t)(y0 - target->rows.first_row) * pitch;
    for (int remaining = y1 - y0; remaining > 0; remaining--) {
        row_span_t span;
        if (clip_row(target, f->last_column, offset, left, right, &span)) {
            if (writing) {
                write_span(target, y1 - remaining, span.first, span.last, (int32_t)f->flat_z << R3D_DEPTH_SHIFT, 0);
            } else {
                fill_flat(span.depth, span.color, span.last - span.first + 1, f->flat_z, f->flat_color);
            }
        }
        step_edge(left);
        step_edge(right);
        offset += pitch;
    }
}

static inline __attribute__((always_inline)) void
fill_carried_span(const r3d_span_target_t* target, const fill_t* f, const uint32_t row[ATTRIBUTES], int y, int column,
                  const row_span_t* span, int n, bool writing) {
    const gradients_t* g = f->g;
    if (n == 1) {
        int32_t z;
        int32_t dz;
        span_start(g, row[0], 0, column < 0 ? -column : 0, span->last - span->first, &z, &dz);
        if (writing) {
            write_span(target, y, span->first, span->last, z, dz);
        } else {
            fill_solid(span->depth, span->color, span->last - span->first + 1, z, dz, f->flat_color);
        }
    } else if (g->in_range && column >= 0) {
        fill_gouraud(span->depth, span->color, span->last - span->first + 1, row, g->dx);
    } else {
        fill_clipped_gouraud(g, row, column, span);
    }
}

static inline __attribute__((always_inline)) void
carried_rows(const r3d_span_target_t* target, const fill_t* f, uint32_t row[ATTRIBUTES], int y0, int y1, edge_t* left,
             edge_t* right, int n, bool writing) {
    const gradients_t* g = f->g;
    uint32_t increment[ATTRIBUTES];
    uint32_t extra_increment[ATTRIBUTES];
    for (int k = 0; k < n; k++) {
        increment[k] = (uint32_t)g->dy[k] + ((uint32_t)g->dx[k] * (uint32_t)left->qs);
        extra_increment[k] = increment[k] + (uint32_t)g->dx[k];
    }
    const size_t pitch = target->rows.attachment[GFX_ATTACHMENT_COLOR].pitch;
    size_t offset = (size_t)(y0 - target->rows.first_row) * pitch;
    for (int remaining = y1 - y0; remaining > 0; remaining--) {
        const int y = y1 - remaining;
        row_span_t span;
        if (clip_row(target, f->last_column, offset, left, right, &span)) {
            fill_carried_span(target, f, row, y, left->q, &span, n, writing);
        }
        const uint32_t* advance = increment;
        if (step_edge(left)) {
            advance = extra_increment;
        }
        /* n is 1 or ATTRIBUTES. Written-out adds avoid a hardware loop
         * that recomputes its trip count every row. */
        row[0] += advance[0];
        if (n == ATTRIBUTES) {
            row[1] += advance[1];
            row[2] += advance[2];
            row[3] += advance[3];
        }
        step_edge(right);
        offset += pitch;
    }
}

/* Unsigned carries preserve extrapolated planes until a visible start. */
static RENDER_IRAM_ATTR void
walk_rows(const r3d_span_target_t* target, const fill_t* f, uint32_t row[ATTRIBUTES], int y0, int y1, edge_t* left_edge,
          edge_t* right_edge) {
    edge_t left = *left_edge;
    edge_t right = *right_edge;
    if (r3d_span_stop_after == 2) {
        for (int y = y0; y < y1; y++) {
            step_edge(&left);
            step_edge(&right);
        }
    } else if (f->shading == FILL_FLAT) {
        flat_rows(target, f, y0, y1, &left, &right, false);
    } else if (f->shading == FILL_FACE) {
        carried_rows(target, f, row, y0, y1, &left, &right, 1, false);
    } else {
        carried_rows(target, f, row, y0, y1, &left, &right, ATTRIBUTES, false);
    }
    *left_edge = left;
    *right_edge = right;
}

/* Every writer of the target over one span. */
static void
write_span(const r3d_span_target_t* target, int y, int x_first, int x_last, int32_t z, int32_t dz) {
    for (int i = 0; i < target->writer_count; i++) {
        target->writers[i].span(&target->writers[i], &target->rows, y, x_first, x_last, z, dz);
    }
}

/* The target's writers over the spans walk_rows() just filled, from the same
 * edges and depth plane, so they see each pixel's depth as it was tested. */
static void
writer_rows(const r3d_span_target_t* target, const fill_t* f, uint32_t row[ATTRIBUTES], int y0, int y1, edge_t* left,
            edge_t* right) {
    if (f->shading == FILL_FLAT) {
        flat_rows(target, f, y0, y1, left, right, true);
    } else {
        carried_rows(target, f, row, y0, y1, left, right, 1, true);
    }
}

typedef struct {
    const r3d_span_vertex_t *v0, *v1, *v2; /* by y */
    int y_first, split, y_end;
    bool long_on_left;
} walk_t;

/* One stretch of rows between two edges, filled, or walked for the
 * target's writers when `writing`. */
static inline __attribute__((always_inline)) void
rows_between(const r3d_span_target_t* target, const fill_t* f, uint32_t row[ATTRIBUTES], int y0, int y1,
             edge_t* long_edge, edge_t* short_edge, bool writing, bool long_on_left) {
    edge_t* left = long_on_left ? long_edge : short_edge;
    edge_t* right = long_on_left ? short_edge : long_edge;
    if (writing) {
        writer_rows(target, f, row, y0, y1, left, right);
    } else {
        walk_rows(target, f, row, y0, y1, left, right);
    }
}

static inline __attribute__((always_inline)) int
carried_attributes(const fill_t* f, bool writing) {
    if (f->shading == FILL_FLAT || (!writing && r3d_span_stop_after == 2)) {
        return 0;
    }
    return writing || f->shading == FILL_FACE ? 1 : ATTRIBUTES;
}

static inline __attribute__((always_inline)) void
shift_carry(const fill_t* f, uint32_t carry[ATTRIBUTES], int attributes, int columns) {
    for (int k = 0; k < attributes; k++) {
        carry[k] += (uint32_t)f->g->dx[k] * (uint32_t)columns;
    }
}

/* The long edge v0-v2 runs the whole height on one side; the short side is
 * v0-v1 above the split and v1-v2 below it. */
static inline __attribute__((always_inline)) void
walk(const r3d_span_target_t* target, const fill_t* f, const walk_t* w, bool writing) {
    edge_t long_edge = edge_at(w->v0, w->v2, w->y_first);
    uint32_t* carry = (uint32_t*)f->g->base;
    const int attributes = carried_attributes(f, writing);
    edge_t short_edge = w->y_first < w->split ? edge_at(w->v0, w->v1, w->y_first) : edge_at(w->v1, w->v2, w->y_first);
    int carry_column = w->long_on_left ? long_edge.q : short_edge.q;
    shift_carry(f, carry, attributes, carry_column - f->g->x_origin);
    if (w->y_first < w->split) {
        rows_between(target, f, carry, w->y_first, w->split, &long_edge, &short_edge, writing, w->long_on_left);
        if (w->split == w->y_end) {
            return;
        }
        carry_column = w->long_on_left ? long_edge.q : short_edge.q;
        short_edge = edge_at(w->v1, w->v2, w->split);
        const int lower_column = w->long_on_left ? long_edge.q : short_edge.q;
        shift_carry(f, carry, attributes, lower_column - carry_column);
    }
    rows_between(target, f, carry, w->split, w->y_end, &long_edge, &short_edge, writing, w->long_on_left);
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

static inline void
set_flat(fill_t* f, const r3d_span_vertex_t* a, const r3d_span_vertex_t* b, const r3d_span_vertex_t* c) {
    const float third = 1.0F / (float)TRIANGLE_VERTICES;
    f->flat_z = (uint16_t)(mathf_clamp((a->z + b->z + c->z) * third, 0.0F, 1.0F) * (float)R3D_DEPTH_NEAREST);
    f->flat_color =
        r3d_span_pack_rgb((a->r + b->r + c->r) * third, (a->g + b->g + c->g) * third, (a->b + b->b + c->b) * third);
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

static inline bool
channel_within_spread(int32_t a, int32_t b, int32_t c, int limit) {
    return r3d_span_max3(a, b, c) - r3d_span_min3(a, b, c) <= limit;
}

static inline bool
colours_within_spread(uint16_t a, uint16_t b, uint16_t c, int limit) {
    a = rgb565_swap(a);
    b = rgb565_swap(b);
    c = rgb565_swap(c);
    return channel_within_spread((int32_t)rgb565_r5(a), (int32_t)rgb565_r5(b), (int32_t)rgb565_r5(c), limit)
           && channel_within_spread((int32_t)rgb565_g6(a), (int32_t)rgb565_g6(b), (int32_t)rgb565_g6(c), limit)
           && channel_within_spread((int32_t)rgb565_b5(a), (int32_t)rgb565_b5(b), (int32_t)rgb565_b5(c), limit);
}

const r3d_span_small_fill_t r3d_span_small_fill_default = {
    .ceiling = SMALL_FILL_CEILING_SMOOTH,
    .style = SMALL_FILL_FLAT,
};

void
r3d_span_small_fill_resolve(r3d_span_small_fill_t* resolved, small_fill_t fill) {
    fill.ceiling = (uint8_t)mathi_min(fill.ceiling, SMALL_FILL_CEILING_MAX);
    resolved->ceiling = fill.ceiling;
    resolved->style = fill.style;
    const int intervals = fill.ceiling - (SMALL_MAX_SIDE + 1);
    for (int side = SMALL_MAX_SIDE + 1; side <= fill.ceiling; side++) {
        const int offset = side - (SMALL_MAX_SIDE + 1);
        const int spread = fill.gate == 0 ? SMALL_SPREAD_OFF
                           : intervals == 0
                               ? fill.gate
                               : SMALL_SPREAD_OFF + ((((int)fill.gate - SMALL_SPREAD_OFF) * offset) / intervals);
        resolved->spread[side] = (uint8_t)spread;
    }
}

/* Colour conversion stays in flash; the side and gate checks stay on the draw path. */
static __attribute__((noinline)) bool
small_colours_within_spread(const r3d_span_vertex_t* a, const r3d_span_vertex_t* b, const r3d_span_vertex_t* c,
                            int spread) {
    return colours_within_spread(r3d_span_pack_rgb(a->r, a->g, a->b), r3d_span_pack_rgb(b->r, b->g, b->b),
                                 r3d_span_pack_rgb(c->r, c->g, c->b), spread);
}

/* Negative selects plane shading; otherwise the result names a style kernel. */
static inline __attribute__((always_inline)) int
select_small(const r3d_span_vertex_t* a, const r3d_span_vertex_t* b, const r3d_span_vertex_t* c, recti_t centres,
             bool solid, const r3d_span_small_fill_t* small_fill) {
    const int side = mathi_max(centres.w, centres.h);
    if (side > small_fill->ceiling) {
        return SMALL_FILL_WALK;
    }
    if (solid) {
        return SMALL_FILL_FLAT;
    }
    if (side <= SMALL_MAX_SIDE) {
        return SMALL_FILL_FLAT;
    }
    const int spread = small_fill->spread[side];
    if (spread != SMALL_SPREAD_OFF && !small_colours_within_spread(a, b, c, spread)) {
        return SMALL_FILL_WALK;
    }
    return small_fill->style;
}

static inline __attribute__((always_inline)) bool
small_visible(const small_edge_t edges[TRIANGLE_VERTICES], const pixel_row_t* row, int x, uint16_t depth) {
    const int32_t cx = (x << R3D_SUBPIXEL_SHIFT) + HALF_PIXEL;
    return (small_side(&edges[0], cx, row->centre) | small_side(&edges[1], cx, row->centre)
            | small_side(&edges[2], cx, row->centre))
               >= 0
           && depth > row->depth[x];
}

/* Pineda 1988 [32]: test pixel centres against three edge functions.
 * Keep small-triangle temporaries off the row-walk call chain. */
static __attribute__((noinline)) void
fill_small(const r3d_span_target_t* target, const r3d_span_vertex_t* a, const r3d_span_vertex_t* b,
           const r3d_span_vertex_t* c, bool positive, recti_t box, const uint16_t* face) {
    fill_t f;
    set_flat(&f, a, b, c);
    if (face != NULL) {
        f.flat_color = *face;
    }
    if (r3d_span_stop_after != 0) {
        return;
    }
    const r3d_span_vertex_t* p = positive ? b : c;
    const r3d_span_vertex_t* q = positive ? c : b;
    const small_edge_t e[TRIANGLE_VERTICES] = {small_edge(a, p), small_edge(p, q), small_edge(q, a)};
    for (int y = box.y; y < recti_bottom(box); y++) {
        const pixel_row_t row = pixel_row(target, y);
        for (int x = box.x, end = recti_right(box); x < end; x++) {
            const int32_t cx = (x << R3D_SUBPIXEL_SHIFT) + HALF_PIXEL;
            if ((small_side(&e[0], cx, row.centre) | small_side(&e[1], cx, row.centre)
                 | small_side(&e[2], cx, row.centre))
                    >= 0
                && f.flat_z > row.depth[x]) {
                row.depth[x] = f.flat_z;
                row.color[x] = f.flat_color;
            }
        }
    }
}

/* A shared channel step preserves hue; mixed endpoints may have no safe step. */
static inline uint16_t
small_grain_shade(uint16_t color) {
    const uint16_t native = rgb565_swap(color);
    const int r = (int)rgb565_r5(native);
    const int g = (int)rgb565_g6(native);
    const int b = (int)rgb565_b5(native);
    const bool lighter =
        r < SMALL_GRAIN_RB_MAX && g <= (int)RGB565_G6_MAX - SMALL_GRAIN_GREEN_STEP && b < SMALL_GRAIN_RB_MAX;
    const bool darker = r > 0 && g >= SMALL_GRAIN_GREEN_STEP && b > 0;
    const bool dark_half = ((SMALL_GRAIN_GREEN_STEP * (r + b)) + g) < SMALL_GRAIN_MIDPOINT;
    const int step = dark_half ? (lighter ? 1 : darker ? -1 : 0) : (darker ? -1 : lighter ? 1 : 0);
    return rgb565be_pack((uint8_t)(r + step), (uint8_t)(g + (SMALL_GRAIN_GREEN_STEP * step)), (uint8_t)(b + step));
}

static inline __attribute__((always_inline)) void
small_store(const small_edge_t edges[TRIANGLE_VERTICES], const pixel_row_t* row, int x, uint16_t depth,
            uint16_t color) {
    if (small_visible(edges, row, x, depth)) {
        row->depth[x] = depth;
        row->color[x] = color;
    }
}

static __attribute__((noinline)) void
fill_small_grain(const r3d_span_target_t* target, const r3d_span_vertex_t* a, const r3d_span_vertex_t* b,
                 const r3d_span_vertex_t* c, bool positive, recti_t box) {
    fill_t f;
    set_flat(&f, a, b, c);
    const uint16_t shade = small_grain_shade(f.flat_color);
    if (r3d_span_stop_after != 0) {
        return;
    }
    const small_edge_t edges[TRIANGLE_VERTICES] = {small_edge(a, positive ? b : c),
                                                   small_edge(positive ? b : c, positive ? c : b),
                                                   small_edge(positive ? c : b, a)};
    for (int y = box.y; y < recti_bottom(box); y++) {
        const pixel_row_t row = pixel_row(target, y);
        const uint16_t first = ((box.x ^ y) & 1) == 0 ? f.flat_color : shade;
        const uint16_t second = ((box.x ^ y) & 1) == 0 ? shade : f.flat_color;
        int x = box.x;
        const int end = recti_right(box);
        for (; x + 1 < end; x += SMALL_GRAIN_PAIR) {
            small_store(edges, &row, x, f.flat_z, first);
            small_store(edges, &row, x + 1, f.flat_z, second);
        }
        if (x < end) {
            small_store(edges, &row, x, f.flat_z, first);
        }
    }
}

static inline __attribute__((always_inline)) uint16_t
small_dither_pick(const uint16_t colors[TRIANGLE_VERTICES], uint16_t flat, int32_t threshold, int32_t s0, int32_t s1,
                  int32_t s2) {
    if (s0 == s1 && s1 == s2) {
        return flat;
    }
    return colors[threshold < s0 ? 0 : threshold < s0 + s1 ? 1 : 2];
}

/* Barycentric ordered picks share the screen-anchored graphics pattern. */
static __attribute__((noinline)) bool
fill_small_dither(const r3d_span_target_t* target, const r3d_span_vertex_t* a, const r3d_span_vertex_t* b,
                  const r3d_span_vertex_t* c, bool positive, recti_t box) {
    const uint16_t colors[TRIANGLE_VERTICES] = {
        r3d_span_pack_rgb(a->r, a->g, a->b), r3d_span_pack_rgb(b->r, b->g, b->b), r3d_span_pack_rgb(c->r, c->g, c->b)};
    if (colors[0] == colors[1] && colors[1] == colors[2]) {
        return true;
    }
    const r3d_span_vertex_t* p = positive ? b : c;
    const r3d_span_vertex_t* q = positive ? c : b;
    const small_edge_t edges[TRIANGLE_VERTICES] = {small_edge(a, p), small_edge(p, q), small_edge(q, a)};
    const uint16_t picks[TRIANGLE_VERTICES] = {colors[positive ? 2 : 1], colors[0], colors[positive ? 1 : 2]};
    const int32_t total =
        small_side(&edges[0], a->x, a->y) + small_side(&edges[1], a->x, a->y) + small_side(&edges[2], a->x, a->y);
    fill_t f;
    set_flat(&f, a, b, c);
    if (r3d_span_stop_after != 0) {
        return false;
    }
    for (int y = box.y; y < recti_bottom(box); y++) {
        const pixel_row_t row = pixel_row(target, y);
        const uint8_t* pattern = bayer4x4[y & SMALL_DITHER_MASK];
        for (int x = box.x; x < recti_right(box); x++) {
            const int32_t cx = (x << R3D_SUBPIXEL_SHIFT) + HALF_PIXEL;
            const int32_t s0 = small_side(&edges[0], cx, row.centre);
            const int32_t s1 = small_side(&edges[1], cx, row.centre);
            const int32_t s2 = small_side(&edges[2], cx, row.centre);
            if ((s0 | s1 | s2) >= 0 && f.flat_z > row.depth[x]) {
                const int32_t threshold = (total * (2 * pattern[x & SMALL_DITHER_MASK] + 1)) >> SMALL_DITHER_SHIFT;
                row.depth[x] = f.flat_z;
                row.color[x] = small_dither_pick(picks, f.flat_color, threshold, s0, s1, s2);
            }
        }
    }
    return false;
}

static __attribute__((noinline)) void
fill_small_styled(const r3d_span_target_t* target, const r3d_span_vertex_t* a, const r3d_span_vertex_t* b,
                  const r3d_span_vertex_t* c, bool positive, recti_t box, int style) {
    if (style == SMALL_FILL_GRAIN) {
        fill_small_grain(target, a, b, c, positive, box);
    } else if (fill_small_dither(target, a, b, c, positive, box)) {
        fill_small(target, a, b, c, positive, box, NULL);
    }
}

static inline __attribute__((always_inline)) void
plane_limits(int32_t top, int32_t dx, int32_t dy, recti_t box, int64_t* low, int64_t* high) {
    const int64_t across = (int64_t)dx * (box.w - 1);
    const int64_t bottom = top + ((int64_t)dy * (box.h - 1));
    *low = (top < bottom ? top : bottom) + (across < 0 ? across : 0);
    *high = (top > bottom ? top : bottom) + (across > 0 ? across : 0);
}

/* A span runs from its clamped start to its end, both values of the plane
 * at centres in `box`, so the largest corner bounds it; a start clamped up
 * from below zero lifts the span by at most the lowest corner's shortfall.
 * The corners lie past where the walk steps, so the sums need 64 bits. */
int32_t
r3d_span_plane_bound(int32_t top, int32_t dx, int32_t dy, recti_t box) {
    int64_t low;
    int64_t high;
    plane_limits(top, dx, dy, box, &low, &high);
    const int64_t lifted = high - (low < 0 ? low : 0);
    return (int32_t)((lifted < value_max[0] ? lifted : value_max[0]) >> R3D_DEPTH_SHIFT);
}

bool
r3d_span_plane_in_range(int32_t top, int32_t dx, int32_t dy, int32_t max, recti_t box) {
    int64_t low;
    int64_t high;
    plane_limits(top, dx, dy, box, &low, &high);
    return low >= 0 && high <= max;
}

RENDER_IRAM_ATTR bool
r3d_span_hidden(const r3d_span_target_t* target, int32_t bound, recti_t box) {
    for (int y = box.y; y < recti_bottom(box); y++) {
        const uint16_t* depth = gfx_target_depth(&target->rows, y);
        for (int x = box.x, end = recti_right(box); x < end; x++) {
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
set_up_planes(const fill_t* f, const r3d_span_vertex_t* const v[TRIANGLE_VERTICES], const plane_t* p, recti_t box,
              int y_anchor, gradients_t* g) {
    g->in_range = r3d_span_plane_in_range(g->base[0], g->dx[0], g->dy[0], value_max[0], box);
    if (f->shading == FILL_FACE) {
        return;
    }
    colour_gradients(p, v[0], v[1], v[2], g);
    for (int k = 1; k < ATTRIBUTES; k++) {
        g->base[k] = first_row_value(g, k, box.y, y_anchor);
        g->in_range = g->in_range && r3d_span_plane_in_range(g->base[k], g->dx[k], g->dy[k], value_max[k], box);
    }
}

/* The depth plane first, then the colour planes only for a triangle not
 * already hidden; false when it would write nothing. Setup temporaries
 * must leave the stack before the row walk. */
static __attribute__((noinline)) bool
set_up_fill(const r3d_span_target_t* target, const r3d_span_vertex_t* const v[TRIANGLE_VERTICES], recti_t box,
            int y_anchor, const uint16_t* face, fill_t* f, gradients_t* g) {
    plane_t p;
    g->x_origin = box.x;
    if (f->shading == FILL_FLAT) {
        p = (plane_t){0};
        set_flat(f, v[0], v[1], v[2]);
    } else if (plane_of(v[0], v[1], v[2], box.x, y_anchor, &p)) {
        gradient(&p, v[0]->z * DEPTH_SCALE, v[1]->z * DEPTH_SCALE, v[2]->z * DEPTH_SCALE, 0, g);
        g->base[0] = first_row_value(g, 0, box.y, y_anchor);
    } else {
        return false;
    }
    if (face != NULL) {
        f->flat_color = *face;
    }
    const int32_t bound =
        f->shading == FILL_FLAT ? f->flat_z : r3d_span_plane_bound(g->base[0], g->dx[0], g->dy[0], box);
    if (r3d_span_hidden(target, bound, box)) {
        return false;
    }
    if (f->shading != FILL_FLAT) {
        set_up_planes(f, v, &p, box, y_anchor, g);
    }
    return true;
}

static RENDER_IRAM_ATTR void
r3d_span_triangle_impl(const r3d_span_target_t* target, const r3d_span_vertex_t* a, const r3d_span_vertex_t* b,
                       const r3d_span_vertex_t* c, const uint16_t* face) {
    const r3d_span_extent_t e = r3d_span_extent_limits(a, b, c, flat_max_rows, flat_max_width);
    const recti_t box = r3d_span_window_box(target, e.centres);
    if (recti_is_empty(box)) {
        return; /* no pixel centre inside this window */
    }
    const int32_t area2 = r3d_span_area2(a, b, c);
    if (area2 == 0) {
        return;
    }

    const int style = select_small(a, b, c, e.centres, face != NULL, target->small_fill);
    if (style != SMALL_FILL_WALK) {
        FRAME_LAP("r3d.draw.fill");
        if (style == SMALL_FILL_GRAIN || style == SMALL_FILL_DITHER) {
            fill_small_styled(target, a, b, c, area2 > 0, box, style);
        } else {
            fill_small(target, a, b, c, area2 > 0, box, face);
        }
        return;
    }

    fill_t f = {e.flat         ? FILL_FLAT
                : face != NULL ? FILL_FACE
                               : FILL_GOURAUD,
                0, 0, NULL, target->rows.attachment[GFX_ATTACHMENT_COLOR].width - 1};
    /* Attributes anchor at the triangle's first row, or at screen row 0 for
     * one starting above the screen: never at a window's own edge. */
    const int y_anchor = mathi_clamp(e.centres.y, -1, gfx_target_end_row(&target->rows) + 1);
    gradients_t g = {0};
    f.g = &g;
    if (!set_up_fill(target, (const r3d_span_vertex_t* const[TRIANGLE_VERTICES]){a, b, c}, box, y_anchor, face, &f,
                     &g)) {
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
    const walk_t w = {v0,
                      v1,
                      v2,
                      box.y,
                      mathi_clamp(r3d_span_first_centre(v1->y), box.y, recti_bottom(box)),
                      recti_bottom(box),
                      (area2 > 0) != odd};
    FRAME_LAP("r3d.draw.fill");
    walk(target, &f, &w, false);
}

/* The fill's own setup, depth plane only: the same path, box, plane and
 * edges, so each writer sees the depth each pixel was tested with. Not
 * skipped when hidden: the fill has already written its depth. */
void
r3d_span_triangle_write(const r3d_span_target_t* target, const r3d_span_vertex_t* a, const r3d_span_vertex_t* b,
                        const r3d_span_vertex_t* c, bool solid) {
    const r3d_span_vertex_t* v0 = a;
    const r3d_span_vertex_t* v1 = b;
    const r3d_span_vertex_t* v2 = c;
    const bool odd = sort_by_y(&v0, &v1, &v2);
    const r3d_span_extent_t e = r3d_span_extent_limits(a, b, c, flat_max_rows, flat_max_width);
    const recti_t box = r3d_span_window_box(target, e.centres);
    const int32_t area2 = r3d_span_area2(a, b, c);
    if (recti_is_empty(box) || area2 == 0) {
        return;
    }
    /* FILL_FACE selects a depth carry here; writers do not read face colour. */
    fill_t f = {e.flat ? FILL_FLAT : FILL_FACE, 0, 0, NULL, target->rows.attachment[GFX_ATTACHMENT_COLOR].width - 1};
    if (select_small(a, b, c, e.centres, solid, target->small_fill) != SMALL_FILL_WALK) {
        set_flat(&f, a, b, c);
        for (int y = box.y; y < recti_bottom(box); y++) {
            write_span(target, y, box.x, recti_right(box) - 1, (int32_t)f.flat_z << R3D_DEPTH_SHIFT, 0);
        }
        return;
    }
    const int y_anchor = mathi_clamp(e.centres.y, -1, gfx_target_end_row(&target->rows) + 1);
    gradients_t g = {0};
    plane_t p;
    f.g = &g;
    g.x_origin = box.x;
    if (f.shading == FILL_FLAT) {
        set_flat(&f, a, b, c);
    } else if (plane_of(a, b, c, box.x, y_anchor, &p)) {
        gradient(&p, a->z * DEPTH_SCALE, b->z * DEPTH_SCALE, c->z * DEPTH_SCALE, 0, &g);
        g.base[0] = first_row_value(&g, 0, box.y, y_anchor);
        g.in_range = r3d_span_plane_in_range(g.base[0], g.dx[0], g.dy[0], value_max[0], box);
    } else {
        return;
    }
    const walk_t w = {v0,
                      v1,
                      v2,
                      box.y,
                      mathi_clamp(r3d_span_first_centre(v1->y), box.y, recti_bottom(box)),
                      recti_bottom(box),
                      (area2 > 0) != odd};
    walk(target, &f, &w, true);
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
