#include "render/curve/r3d_curve.h"

#include <math.h>
#include "asset/asset_bytes.h"
#include "asset/asset_entry.h"
#include "image/pixel/rgb565.h"
#include "math/geometry/box3f.h"
#include "profile/frame_laps.h"
#include "render/r3d_pipeline.h"
#include "render/r3d_span.h"
#include "render/r3d_span_internal.h"

#define QUAD_CORNERS     4
#define TRIANGLE_CORNERS 3
#define CLIPPED_CORNERS  (QUAD_CORNERS + SCREEN_PLANES)

enum { SCREEN_LEFT, SCREEN_RIGHT, SCREEN_TOP, SCREEN_BOTTOM, SCREEN_PLANES };

#define CURVE_SKIP_PX 1.0F
#define CURVE_HALF    0.5F

_Static_assert(sizeof(r3d_curve_t) == 16, "LCRV curve row layout");
_Static_assert(sizeof(r3d_curve_knot_t) == 10, "LCRV knot row layout");

/* A knot in lens space; its colour stays packed, the ribbon drawing one mean of its two ends. */
typedef struct {
    vec3f_t point;
    float radius;
    uint16_t color;
} knot_t;

typedef struct {
    float x, y, z;
} corner_t;

static knot_t
knot_at(const r3d_curve_knot_t* knot, const r3d_lens_t* lens) {
    return (knot_t){mat4f_apply(&lens->m, (vec3f_t){knot->x, knot->y, knot->z}), (float)knot->radius, knot->color};
}

static knot_t
knot_between(knot_t a, knot_t b, float t) {
    return (knot_t){vec3f_add(a.point, vec3f_scale(vec3f_sub(b.point, a.point), t)),
                    a.radius + (t * (b.radius - a.radius)), a.color};
}

static float
screen_distance(corner_t v, int plane, const r3d_lens_t* lens) {
    switch (plane) {
        case SCREEN_LEFT: return v.x;
        case SCREEN_RIGHT: return (float)lens->width - v.x;
        case SCREEN_TOP: return v.y;
        default: return (float)lens->height - v.y;
    }
}

/* Screen clipping precedes snapping so near-plane ribbons stay within span arithmetic's range. Out of line:
 * its scratch corners are gone before the ribbon's triangles are drawn. */
static __attribute__((noinline)) int
clip_ribbon(corner_t vertices[CLIPPED_CORNERS], const r3d_lens_t* lens) {
    corner_t next[CLIPPED_CORNERS];
    int count = QUAD_CORNERS;
    for (int plane = 0; plane < SCREEN_PLANES && count > 0; plane++) {
        int kept = 0;
        for (int i = 0; i < count; i++) {
            const corner_t a = vertices[i];
            const corner_t b = vertices[(i + 1) % count];
            const float da = screen_distance(a, plane, lens);
            const float db = screen_distance(b, plane, lens);
            if ((da >= 0) != (db >= 0)) {
                const float t = da / (da - db);
                next[kept++] = (corner_t){a.x + (t * (b.x - a.x)), a.y + (t * (b.y - a.y)), a.z + (t * (b.z - a.z))};
            }
            if (db >= 0) {
                next[kept++] = b;
            }
        }
        count = kept;
        for (int i = 0; i < count; i++) {
            vertices[i] = next[i];
        }
    }
    return count;
}

/* Colour goes to the fill as one packed value, so the vertex carries none. */
static r3d_span_vertex_t
span_vertex(corner_t corner) {
    return (r3d_span_vertex_t){
        .x = (int32_t)lroundf(corner.x * R3D_SUBPIXEL), .y = (int32_t)lroundf(corner.y * R3D_SUBPIXEL), .z = corner.z};
}

/* Whether a ribbon's corners all lie above or all below this core's rows, so it fills none of them. */
static bool
ribbon_misses_rows(const corner_t corners[QUAD_CORNERS], const r3d_span_target_t* target) {
    const float top = (float)target->rows.first_row;
    const float bottom = (float)gfx_target_end_row(&target->rows);
    bool above = true;
    bool below = true;
    for (int i = 0; i < QUAD_CORNERS; i++) {
        above = above && corners[i].y < top;
        below = below && corners[i].y > bottom;
    }
    return above || below;
}

/* Cuts the segment a-b at the near plane; false when both ends lie in front of it. */
static bool
clip_near(knot_t* a, knot_t* b, float near) {
    if (a->point.z < near && b->point.z < near) {
        return false;
    }
    if (a->point.z < near) {
        *a = knot_between(*a, *b, (near - a->point.z) / (b->point.z - a->point.z));
        a->point.z = near;
    }
    if (b->point.z < near) {
        *b = knot_between(*b, *a, (near - b->point.z) / (a->point.z - b->point.z));
        b->point.z = near;
    }
    return true;
}

/* Out of line: the curve loop's knots stay off the ribbon's clip and fill. */
static __attribute__((noinline)) void
segment(const knot_t* from, const knot_t* to, float radius_scale, const r3d_lens_t* lens,
        const r3d_span_target_t* target, bool covered) {
    knot_t a = *from;
    knot_t b = *to;
    if (!clip_near(&a, &b, lens->project.near_z)) {
        return;
    }
    /* Lens space already points y down the picture, as r3d_pipeline's vertices take it. */
    const float ax = lens->project.cx + (a.point.x / a.point.z);
    const float ay = lens->project.cy + (a.point.y / a.point.z);
    const float bx = lens->project.cx + (b.point.x / b.point.z);
    const float by = lens->project.cy + (b.point.y / b.point.z);
    const float dx = bx - ax;
    const float dy = by - ay;
    const float length = hypotf(dx, dy);
    const float ar = a.radius * radius_scale / a.point.z;
    const float br = b.radius * radius_scale / b.point.z;
    float ah = fmaxf(ar, R3D_CURVE_MIN_WIDTH_PX * CURVE_HALF);
    float bh = fmaxf(br, R3D_CURVE_MIN_WIDTH_PX * CURVE_HALF);
    const float coverage = fminf(1.0F, (ar + br) / (ah + bh));
    if ((coverage < 1.0F) != covered) {
        return;
    }
    float az = fminf(1.0F, lens->depth_near / a.point.z);
    float bz = fminf(1.0F, lens->depth_near / b.point.z);
    const bool end_on = length < CURVE_SKIP_PX;
    if (end_on) {
        /* Both ends on one pixel: one square as wide as the wider end, at the nearer end's depth. */
        ah = bh = fmaxf(ah, bh);
        az = bz = fmaxf(az, bz);
    }
    const float ux = end_on ? 1.0F : dx / length;
    const float uy = end_on ? 0.0F : dy / length;
    const float ex = end_on ? ax : bx;
    const float ey = end_on ? ay : by;
    corner_t corners[CLIPPED_CORNERS] = {
        {ax - (ux * ah) - (uy * ah), ay - (uy * ah) + (ux * ah), az},
        {ax - (ux * ah) + (uy * ah), ay - (uy * ah) - (ux * ah), az},
        {ex + (ux * bh) + (uy * bh), ey + (uy * bh) - (ux * bh), bz},
        {ex + (ux * bh) - (uy * bh), ey + (uy * bh) + (ux * bh), bz},
    };
    if (ribbon_misses_rows(corners, target)) {
        return;
    }
    const int count = clip_ribbon(corners, lens);
    if (count < TRIANGLE_CORNERS) {
        return;
    }
    const uint16_t color = rgb565be_blend(a.color, b.color, RGB565_SCALE_UNIT / 2);
    FRAME_LAP("r3d.curve.fill");
    /* A fan from the first corner, three vertices at a time: no array of them on the stack. */
    const r3d_span_vertex_t first = span_vertex(corners[0]);
    r3d_span_vertex_t previous = span_vertex(corners[1]);
    for (int i = 2; i < count; i++) {
        const r3d_span_vertex_t next = span_vertex(corners[i]);
        if (covered) {
            r3d_span_triangle_cover(target, &first, &previous, &next, color,
                                    (unsigned)lroundf(coverage * R3D_SPAN_COVERAGE_FULL));
        } else {
            r3d_span_triangle_solid(target, &first, &previous, &next, color);
        }
        previous = next;
    }
    FRAME_LAP("r3d.curve.setup");
}

/* Whether a curve's box reaches this core's rows of the view, a ribbon's widening to its least width beside;
 * the planes stay off the segment draw's stack. */
static __attribute__((noinline)) bool
curve_in_view(const r3d_curve_t* curve, const r3d_lens_t* lens, const r3d_span_target_t* target) {
    planef_t planes[PLANE_FRUSTUM_COUNT];
    planef_frustum(&lens->m, &lens->project,
                   rectf_from_edges(0.0F, (float)target->rows.first_row - R3D_CURVE_MIN_WIDTH_PX, (float)lens->width,
                                    (float)gfx_target_end_row(&target->rows) + R3D_CURVE_MIN_WIDTH_PX),
                   planes);
    unsigned mask = PLANE_FRUSTUM_ALL;
    return !box3f_outside_planes(box3f_from_box3s(r3d_curve_box(curve), 1.0F), planes, PLANE_FRUSTUM_COUNT, &mask);
}

/* Each curve's knots follow the last curve's, at least two, every knot's tube inside the curve's box. */
static bool
curves_are_inside(r3d_curve_set_t set, unsigned knots) {
    unsigned end = 0;
    for (int i = 0; i < set.curve_count; i++) {
        const r3d_curve_t* row = &set.curves[i];
        if (row->first != end || row->count < 2 || (unsigned)row->first + row->count > knots) {
            return false;
        }
        end = (unsigned)row->first + row->count;
        for (unsigned k = row->first; k < end; k++) {
            const r3d_curve_knot_t* knot = &set.knots[k];
            const int16_t position[3] = {knot->x, knot->y, knot->z};
            for (int axis = 0; axis < 3; axis++) {
                if ((int)position[axis] - knot->radius < row->lo[axis]
                    || (int)position[axis] + knot->radius > row->hi[axis]) {
                    return false;
                }
            }
        }
    }
    return end == knots;
}

asset_status_t
r3d_curve_set_from_asset(asset_view_t asset, r3d_curve_set_t* out) {
    *out = (r3d_curve_set_t){0};
    asset_status_t status = asset_entry_validate(asset, R3D_CURVE_VERSION, R3D_CURVE_AT_VERSION, R3D_CURVE_HEADER_SIZE,
                                                 R3D_CURVE_ENTRY_ALIGNMENT);
    if (status != ASSET_OK) {
        return status;
    }
    if (asset_read_u16(asset.data + R3D_CURVE_AT_RESERVED) != 0) {
        return ASSET_ERR_FORMAT;
    }
    const unsigned count = asset_read_u16(asset.data + R3D_CURVE_AT_COUNT);
    const unsigned knots = asset_read_u16(asset.data + R3D_CURVE_AT_KNOT_COUNT);
    const size_t at = R3D_CURVE_HEADER_SIZE + (count * sizeof(r3d_curve_t));
    if (at + (knots * sizeof(r3d_curve_knot_t)) != asset.size) {
        return ASSET_ERR_BOUNDS;
    }
    const r3d_curve_set_t set =
        r3d_curve_set_at((const r3d_curve_t*)(const void*)(asset.data + R3D_CURVE_HEADER_SIZE), (int)count);
    if (!curves_are_inside(set, knots)) {
        return ASSET_ERR_BOUNDS;
    }
    *out = set;
    return ASSET_OK;
}

void
r3d_curve_draw(r3d_curve_set_t set, const r3d_lens_t* lens, const r3d_span_target_t* target, bool covered) {
    if (set.curve_count == 0) {
        return;
    }
    const float* x = lens->m.m[0];
    const float* y = lens->m.m[1];
    const float radius_scale = fmaxf(sqrtf((x[0] * x[0]) + (x[1] * x[1]) + (x[2] * x[2])),
                                     sqrtf((y[0] * y[0]) + (y[1] * y[1]) + (y[2] * y[2])));
    for (int i = 0; i < set.curve_count; i++) {
        const r3d_curve_t* curve = &set.curves[i];
        if (curve->count < 2 || !curve_in_view(curve, lens, target)) {
            continue;
        }
        knot_t a = knot_at(&set.knots[curve->first], lens);
        for (int k = 1; k < curve->count; k++) {
            const knot_t b = knot_at(&set.knots[curve->first + k], lens);
            if (k + 1 < curve->count && a.point.z >= lens->project.near_z && b.point.z >= lens->project.near_z
                && hypotf((a.point.x / a.point.z) - (b.point.x / b.point.z),
                          (a.point.y / a.point.z) - (b.point.y / b.point.z))
                       < CURVE_SKIP_PX) {
                continue;
            }
            segment(&a, &b, radius_scale, lens, target, covered);
            a = b;
        }
    }
}
