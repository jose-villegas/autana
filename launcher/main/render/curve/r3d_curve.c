#include "render/curve/r3d_curve.h"

#include <math.h>
#include "image/pixel/rgb565.h"
#include "math/geometry/box3f.h"
#include "render/r3d_pipeline.h"
#include "render/r3d_span.h"
#include "render/r3d_span_internal.h"

#define QUAD_CORNERS     4
#define TRIANGLE_CORNERS 3
#define CLIPPED_CORNERS  (QUAD_CORNERS + SCREEN_PLANES)

enum { SCREEN_LEFT, SCREEN_RIGHT, SCREEN_TOP, SCREEN_BOTTOM, SCREEN_PLANES };

#define CURVE_SKIP_PX     1.0F
#define CURVE_HALF        0.5F
#define COLOR_RED_SHIFT   16
#define COLOR_GREEN_SHIFT 8
#define COLOR_MASK        UINT8_MAX

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
                    a.radius + t * (b.radius - a.radius), a.color};
}

static float
channel(uint32_t rgb, unsigned shift) {
    return (float)((rgb >> shift) & COLOR_MASK);
}

static uint16_t
mean_color(uint16_t a, uint16_t b) {
    const uint32_t x = rgb565be_to_rgb888(a);
    const uint32_t y = rgb565be_to_rgb888(b);
    return r3d_span_pack_rgb((channel(x, COLOR_RED_SHIFT) + channel(y, COLOR_RED_SHIFT)) * CURVE_HALF,
                             (channel(x, COLOR_GREEN_SHIFT) + channel(y, COLOR_GREEN_SHIFT)) * CURVE_HALF,
                             (channel(x, 0) + channel(y, 0)) * CURVE_HALF);
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
                next[kept++] = (corner_t){a.x + t * (b.x - a.x), a.y + t * (b.y - a.y), a.z + t * (b.z - a.z)};
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

static r3d_span_vertex_t
span_vertex(corner_t corner, uint32_t rgb) {
    return (r3d_span_vertex_t){(int32_t)lroundf(corner.x * R3D_SUBPIXEL),
                               (int32_t)lroundf(corner.y * R3D_SUBPIXEL),
                               corner.z,
                               channel(rgb, COLOR_RED_SHIFT),
                               channel(rgb, COLOR_GREEN_SHIFT),
                               channel(rgb, 0)};
}

/* Out of line: the curve loop's knots stay off the ribbon's clip and fill. */
static __attribute__((noinline)) void
segment(const knot_t* from, const knot_t* to, float radius_scale, const r3d_lens_t* lens,
        const r3d_span_target_t* target) {
    knot_t a = *from;
    knot_t b = *to;
    const float near = lens->project.near_z;
    if (a.point.z < near && b.point.z < near) {
        return;
    }
    if (a.point.z < near) {
        a = knot_between(a, b, (near - a.point.z) / (b.point.z - a.point.z));
        a.point.z = near;
    }
    if (b.point.z < near) {
        b = knot_between(b, a, (near - b.point.z) / (a.point.z - b.point.z));
        b.point.z = near;
    }
    /* Lens space already points y down the picture, as r3d_pipeline's vertices take it. */
    const float ax = lens->project.cx + a.point.x / a.point.z;
    const float ay = lens->project.cy + a.point.y / a.point.z;
    const float bx = lens->project.cx + b.point.x / b.point.z;
    const float by = lens->project.cy + b.point.y / b.point.z;
    const float dx = bx - ax, dy = by - ay;
    const float length = hypotf(dx, dy);
    const float ar = a.radius * radius_scale / a.point.z;
    const float br = b.radius * radius_scale / b.point.z;
    const float ah = fmaxf(ar, R3D_CURVE_MIN_WIDTH_PX * CURVE_HALF);
    const float bh = fmaxf(br, R3D_CURVE_MIN_WIDTH_PX * CURVE_HALF);
    const float coverage = fminf(1.0F, (ar + br) / (ah + bh));
    const float ux = length >= CURVE_SKIP_PX ? dx / length : 1.0F;
    const float uy = length >= CURVE_SKIP_PX ? dy / length : 0.0F;
    const float ex = length >= CURVE_SKIP_PX ? bx : ax;
    const float ey = length >= CURVE_SKIP_PX ? by : ay;
    const float az = fminf(1.0F, lens->depth_near / a.point.z);
    const float bz = fminf(1.0F, lens->depth_near / b.point.z);
    corner_t corners[CLIPPED_CORNERS] = {
        {ax - ux * ah - uy * ah, ay - uy * ah + ux * ah, az},
        {ax - ux * ah + uy * ah, ay - uy * ah - ux * ah, az},
        {ex + ux * bh + uy * bh, ey + uy * bh - ux * bh, bz},
        {ex + ux * bh - uy * bh, ey + uy * bh + ux * bh, bz},
    };
    const int count = clip_ribbon(corners, lens);
    const uint16_t color = mean_color(a.color, b.color);
    if (count < TRIANGLE_CORNERS) {
        return;
    }
    const uint32_t rgb = rgb565be_to_rgb888(color);
    /* A fan from the first corner, three vertices at a time: no array of them on the stack. */
    const r3d_span_vertex_t first = span_vertex(corners[0], rgb);
    r3d_span_vertex_t previous = span_vertex(corners[1], rgb);
    for (int i = 2; i < count; i++) {
        const r3d_span_vertex_t next = span_vertex(corners[i], rgb);
        if (coverage >= 1.0F) {
            r3d_span_triangle_solid(target, &first, &previous, &next, color);
        } else {
            r3d_span_triangle_cover(target, &first, &previous, &next,
                                    (unsigned)lroundf(coverage * R3D_SPAN_COVERAGE_FULL));
        }
        previous = next;
    }
}

/* Whether a curve's box reaches the view; the planes stay off the segment draw's stack. */
static __attribute__((noinline)) bool
curve_in_view(const r3d_curve_t* curve, const r3d_lens_t* lens) {
    planef_t planes[PLANE_FRUSTUM_COUNT];
    r3d_lens_frustum(lens, planes);
    const box3f_t box = {{curve->lo[0], curve->lo[1], curve->lo[2]}, {curve->hi[0], curve->hi[1], curve->hi[2]}};
    unsigned mask = (1U << PLANE_FRUSTUM_COUNT) - 1U;
    return !box3f_outside_planes(box, planes, PLANE_FRUSTUM_COUNT, &mask);
}

void
r3d_curve_draw(r3d_curve_set_t set, const r3d_lens_t* lens, const r3d_span_target_t* target) {
    if (set.curve_count == 0) {
        return;
    }
    const float* x = lens->m.m[0];
    const float* y = lens->m.m[1];
    const float radius_scale = fmaxf(sqrtf((x[0] * x[0]) + (x[1] * x[1]) + (x[2] * x[2])),
                                     sqrtf((y[0] * y[0]) + (y[1] * y[1]) + (y[2] * y[2])));
    for (int i = 0; i < set.curve_count; i++) {
        const r3d_curve_t* curve = &set.curves[i];
        if (curve->count < 2 || !curve_in_view(curve, lens)) {
            continue;
        }
        knot_t a = knot_at(&set.knots[curve->first], lens);
        for (int k = 1; k < curve->count; k++) {
            const knot_t b = knot_at(&set.knots[curve->first + k], lens);
            if (k + 1 < curve->count && a.point.z >= lens->project.near_z && b.point.z >= lens->project.near_z
                && hypotf(a.point.x / a.point.z - b.point.x / b.point.z, a.point.y / a.point.z - b.point.y / b.point.z)
                       < CURVE_SKIP_PX) {
                continue;
            }
            segment(&a, &b, radius_scale, lens, target);
            a = b;
        }
    }
}
