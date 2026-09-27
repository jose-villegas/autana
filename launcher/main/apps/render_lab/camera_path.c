#include "camera_path.h"

#include <math.h>

#pragma GCC diagnostic error "-Wdouble-promotion"

static float
segment_seconds(const camera_path_t* path, int i) {
    const lit_vec3_t a = path->points[i].eye;
    const lit_vec3_t b = path->points[(i + 1) % path->count].eye;
    const float dx = b.x - a.x, dy = b.y - a.y, dz = b.z - a.z;
    const float s = sqrtf(dx * dx + dy * dy + dz * dz) / path->units_per_second;
    return s > path->min_segment_seconds ? s : path->min_segment_seconds;
}

static uint32_t
segment_ms(const camera_path_t* path, int i) {
    return (uint32_t)(segment_seconds(path, i) * 1000.0f + 0.5f);
}

uint32_t
camera_path_period_ms(const camera_path_t* path) {
    uint32_t total = 0;
    for (int i = 0; i < path->count; i++) {
        total += segment_ms(path, i);
    }
    return total;
}

static float
catmull_rom(float p0, float p1, float p2, float p3, float u) {
    const float u2 = u * u, u3 = u2 * u;
    return 0.5f
           * (2.0f * p1 + (p2 - p0) * u + (2.0f * p0 - 5.0f * p1 + 4.0f * p2 - p3) * u2
              + (3.0f * p1 - p0 - 3.0f * p2 + p3) * u3);
}

static lit_vec3_t
spline(lit_vec3_t a, lit_vec3_t b, lit_vec3_t c, lit_vec3_t d, float u) {
    return (lit_vec3_t){catmull_rom(a.x, b.x, c.x, d.x, u), catmull_rom(a.y, b.y, c.y, d.y, u),
                        catmull_rom(a.z, b.z, c.z, d.z, u)};
}

void
camera_path_sample(const camera_path_t* path, uint32_t t_ms, lit_vec3_t* eye, lit_vec3_t* forward) {
    t_ms %= camera_path_period_ms(path);
    int i = 0;
    uint32_t length = segment_ms(path, 0);
    while (t_ms >= length) {
        t_ms -= length;
        i++;
        length = segment_ms(path, i);
    }
    const float u = (float)t_ms / (float)length;
    const int n = path->count;
    const camera_waypoint_t* p0 = &path->points[(i + n - 1) % n];
    const camera_waypoint_t* p1 = &path->points[i];
    const camera_waypoint_t* p2 = &path->points[(i + 1) % n];
    const camera_waypoint_t* p3 = &path->points[(i + 2) % n];

    *eye = spline(p0->eye, p1->eye, p2->eye, p3->eye, u);
    const lit_vec3_t target = spline(p0->target, p1->target, p2->target, p3->target, u);
    *forward = (lit_vec3_t){target.x - eye->x, target.y - eye->y, target.z - eye->z};
}
