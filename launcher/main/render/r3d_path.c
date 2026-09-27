#include "render/r3d_path.h"

#include <math.h>

#pragma GCC diagnostic error "-Wdouble-promotion"

static float
segment_seconds(const r3d_path_t* path, int i) {
    const r3d_vec3f_t a = path->points[i].eye;
    const r3d_vec3f_t b = path->points[(i + 1) % path->count].eye;
    const r3d_vec3f_t d = r3d_vec3f_sub(b, a);
    const float s = sqrtf(r3d_vec3f_dot(d, d)) / path->units_per_second;
    return s > path->min_segment_seconds ? s : path->min_segment_seconds;
}

static uint32_t
segment_ms(const r3d_path_t* path, int i) {
    return (uint32_t)((segment_seconds(path, i) * 1000.0F) + 0.5F);
}

uint32_t
r3d_path_period_ms(const r3d_path_t* path) {
    uint32_t total = 0;
    for (int i = 0; i < path->count; i++) {
        total += segment_ms(path, i);
    }
    return total;
}

static float
catmull_rom(float p0, float p1, float p2, float p3, float u) {
    const float u2 = u * u;
    const float u3 = u2 * u;
    return 0.5F
           * (2.0F * p1 + (p2 - p0) * u + (2.0F * p0 - 5.0F * p1 + 4.0F * p2 - p3) * u2
              + (3.0F * p1 - p0 - 3.0F * p2 + p3) * u3);
}

static r3d_vec3f_t
spline(r3d_vec3f_t a, r3d_vec3f_t b, r3d_vec3f_t c, r3d_vec3f_t d, float u) {
    return (r3d_vec3f_t){catmull_rom(a.x, b.x, c.x, d.x, u), catmull_rom(a.y, b.y, c.y, d.y, u),
                         catmull_rom(a.z, b.z, c.z, d.z, u)};
}

void
r3d_path_sample(const r3d_path_t* path, uint32_t t_ms, r3d_vec3f_t* eye, r3d_vec3f_t* forward) {
    t_ms %= r3d_path_period_ms(path);
    int i = 0;
    uint32_t length = segment_ms(path, 0);
    while (t_ms >= length) {
        t_ms -= length;
        i++;
        length = segment_ms(path, i);
    }
    const float u = (float)t_ms / (float)length;
    const int n = path->count;
    const r3d_waypoint_t* p0 = &path->points[(i + n - 1) % n];
    const r3d_waypoint_t* p1 = &path->points[i];
    const r3d_waypoint_t* p2 = &path->points[(i + 1) % n];
    const r3d_waypoint_t* p3 = &path->points[(i + 2) % n];

    *eye = spline(p0->eye, p1->eye, p2->eye, p3->eye, u);
    const r3d_vec3f_t target = spline(p0->target, p1->target, p2->target, p3->target, u);
    *forward = r3d_vec3f_sub(target, *eye);
}
