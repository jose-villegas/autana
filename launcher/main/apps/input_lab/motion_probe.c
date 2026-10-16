#include "apps/input_lab/motion_probe.h"

#include <math.h>
#include <stdio.h>

#include "math/linear/math_const.h"
#include "math/linear/vec_convert.h"
#include "ui/layout/ui_transform.h"

#define DEGREES_PER_RADIAN (MOTION_PROBE_FULL_TURN_DEG / MATH_TAU)
#define FIXED_RADIX        10

motion_probe_t
motion_probe_measure(vec3i_t acceleration) {
    const vec3f_t a = vec3f_from_vec3i(acceleration);
    const vec2f_t plane = vec3f_xy(a);
    const float plane_counts = sqrtf(vec2f_dot(plane, plane));
    int bearing = (int)lroundf(atan2f(a.y, a.x) * DEGREES_PER_RADIAN) % MOTION_PROBE_FULL_TURN_DEG;
    if (bearing < 0) {
        bearing += MOTION_PROBE_FULL_TURN_DEG;
    }
    return (motion_probe_t){atan2f(plane_counts, -a.z) * DEGREES_PER_RADIAN, bearing, plane_counts,
                            sqrtf(vec3f_dot(a, a))};
}

vec2i_t
motion_probe_bubble(int x, int y, int radius, int quarter) {
    const affine2x_t inverse = ui_transform_quarter_turn(-quarter, 0, 0);
    return affine2x_map_pixel(inverse, (vec2i_t){(int)lroundf((float)x * (float)radius / INPUT_ACCEL_ONE_G),
                                                 (int)lroundf((float)y * (float)radius / INPUT_ACCEL_ONE_G)});
}

int
motion_probe_ring_extent(int radius, int row) {
    return (int)lroundf(sqrtf(((float)radius * (float)radius) - ((float)row * (float)row)));
}

void
motion_probe_format_fixed(char* out, size_t len, int value, int digits, bool show_plus) {
    const unsigned magnitude = value < 0 ? 0U - (unsigned)value : (unsigned)value;
    unsigned scale = 1;
    for (int i = 0; i < digits; i++) {
        scale *= FIXED_RADIX;
    }
    if (snprintf(out, len, "%s%u.%0*u",
                 value < 0   ? "-"
                 : show_plus ? "+"
                             : "",
                 magnitude / scale, digits, magnitude % scale)
            < 0
        && len > 0) {
        out[0] = '\0';
    }
}
