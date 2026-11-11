#include "render/view/r3d_triangle_size.h"

#include <math.h>

static float
edge(r3d_size_point_t a, r3d_size_point_t b, float px, float py) {
    return ((b.x - a.x) * (py - a.y)) - ((b.y - a.y) * (px - a.x));
}

float
r3d_size_area2(r3d_size_point_t a, r3d_size_point_t b, r3d_size_point_t c) {
    return edge(a, b, c.x, c.y);
}

/* Top and left edges own their centres; the triangle winds positive. */
static bool
owns(r3d_size_point_t a, r3d_size_point_t b, float px, float py) {
    const float w = edge(a, b, px, py);
    const float dy = b.y - a.y;
    return w > 0.0F || (w == 0.0F && (dy < 0.0F || (dy == 0.0F && b.x > a.x)));
}

static int
first_centre(float lo) {
    return (int)ceilf(lo - 0.5F);
}

recti_t
r3d_size_centre_box(const r3d_size_point_t v[3]) {
    return recti_from_edges(
        first_centre(fminf(v[0].x, fminf(v[1].x, v[2].x))), first_centre(fminf(v[0].y, fminf(v[1].y, v[2].y))),
        first_centre(fmaxf(v[0].x, fmaxf(v[1].x, v[2].x))), first_centre(fmaxf(v[0].y, fmaxf(v[1].y, v[2].y))));
}

static int
clamp_int(int v, int lo, int hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

/* The centres a positive-winding triangle owns, stopping past 4. */
static int
count_centres(const r3d_size_point_t v[3], recti_t b, int width, int height) {
    int covered = 0;
    for (int y = clamp_int(b.y, 0, height); y < clamp_int(recti_bottom(b), 0, height) && covered <= 4; y++) {
        for (int x = clamp_int(b.x, 0, width); x < clamp_int(recti_right(b), 0, width) && covered <= 4; x++) {
            const float px = (float)x + 0.5F;
            const float py = (float)y + 0.5F;
            covered += owns(v[0], v[1], px, py) && owns(v[1], v[2], px, py) && owns(v[2], v[0], px, py);
        }
    }
    return covered;
}

int
r3d_size_bin(const r3d_size_point_t v[3], recti_t box, int width, int height) {
    const int covered = recti_is_empty(box) ? 0 : count_centres(v, box, width, height);
    return covered == 0   ? R3D_SIZES_ZERO
           : covered == 1 ? R3D_SIZES_ONE
           : covered <= 4 ? R3D_SIZES_TWO_TO_FOUR
                          : R3D_SIZES_MORE;
}
