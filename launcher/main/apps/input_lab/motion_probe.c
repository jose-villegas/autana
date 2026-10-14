#include <math.h>

#include "apps/input_lab/motion_probe.h"
#include "math/linear/vec3f.h"
#include "ui/ui_transform.h"

motion_probe_t
motion_probe_measure(vec3i_t acceleration) {
    const vec3f_t a = {(float)acceleration.x, (float)acceleration.y, (float)acceleration.z};
    const vec2f_t plane = vec3f_xy(a);
    const float plane_counts = sqrtf(vec2f_dot(plane, plane));
    float bearing = plane_counts > 0 ? atan2f(a.y, a.x) * MOTION_PROBE_DEGREES_PER_RADIAN : 0;
    if (bearing < 0) {
        bearing += MOTION_PROBE_FULL_TURN_DEG;
    }
    return (motion_probe_t){atan2f(plane_counts, -a.z) * MOTION_PROBE_DEGREES_PER_RADIAN, bearing, plane_counts,
                            sqrtf(vec3f_dot(a, a))};
}

vec2i_t
motion_probe_bubble(int x, int y, int radius, int quarter) {
    ui_transform_t inverse;
    const ui_transform_t panel = ui_transform_quarter_turn(quarter, 0, 0);
    if (!ui_transform_invert(panel, &inverse)) {
        return (vec2i_t){0};
    }
    int ox, oy;
    ui_transform_point(inverse, (int)lroundf((float)x * radius / INPUT_ACCEL_ONE_G),
                       (int)lroundf((float)y * radius / INPUT_ACCEL_ONE_G), &ox, &oy);
    return (vec2i_t){ox, oy};
}

int
motion_probe_ring_extent(int radius, int row) {
    return (int)lroundf(sqrtf((float)radius * radius - (float)row * row));
}
