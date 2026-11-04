#include "apps/input_lab/motion_probe.h"

#include <math.h>
#include <stdio.h>

#include "apps/input_lab/imu_probe.h"
#include "display/display.h"
#include "math/linear/vec_convert.h"
#include "math/scalar/math_const.h"
#include "ui/layout/ui_transform.h"

#define FIXED_RADIX 10

motion_probe_t
motion_probe_measure(vec3i_t acceleration) {
    const vec3f_t a = vec3f_from_vec3i(acceleration);
    const vec2f_t plane = vec3f_xy(a);
    const float plane_counts = sqrtf(vec2f_dot(plane, plane));
    int bearing = (int)lroundf(atan2f(a.y, a.x) * MATH_DEGREES_PER_RADIAN) % MOTION_PROBE_FULL_TURN_DEG;
    if (bearing < 0) {
        bearing += MOTION_PROBE_FULL_TURN_DEG;
    }
    return (motion_probe_t){atan2f(plane_counts, -a.z) * MATH_DEGREES_PER_RADIAN, bearing, plane_counts,
                            sqrtf(vec3f_dot(a, a))};
}

vec2i_t
motion_probe_bubble(int x, int y, int radius, int quarter) {
    const affine2x_t inverse = ui_transform_quarter_turn(-quarter, 0, 0);
    return affine2x_map_pixel(&inverse, (vec2i_t){(int)lroundf((float)x * (float)radius / INPUT_ACCEL_ONE_G),
                                                  (int)lroundf((float)y * (float)radius / INPUT_ACCEL_ONE_G)});
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

void
motion_probe_format_gravity(char* out, size_t len, const gravity_t* gravity, int quarter) {
    const char quarter_digit = quarter >= 0 && quarter < DISPLAY_QUARTER_COUNT ? (char)('0' + quarter) : '?';
    if (!gravity) {
        (void)snprintf(out, len, "GRAV -- Q%c", quarter_digit);
        return;
    }

    enum { GRAV_X, GRAV_Y, GRAV_Z, GRAV_STRENGTH, GRAV_BIAS_X, GRAV_BIAS_Y, GRAV_BIAS_Z, GRAV_COUNT };

    const float gravity_values[GRAV_COUNT] = {gravity_down(gravity).x,      gravity_down(gravity).y,
                                              gravity_down(gravity).z,      gravity_strength(gravity),
                                              gravity_gyro_bias(gravity).x, gravity_gyro_bias(gravity).y,
                                              gravity_gyro_bias(gravity).z};
    char gravity_text[GRAV_COUNT][MOTION_G_TEXT_SIZE];
    for (size_t i = 0; i < GRAV_COUNT; i++) {
        const bool bias = i >= GRAV_BIAS_X;
        motion_probe_format_fixed(
            gravity_text[i], sizeof gravity_text[i],
            (int)lroundf(gravity_values[i] * (float)(bias ? MOTION_PROBE_OFFSET_SCALE : IMU_PROBE_MILLI)),
            bias ? MOTION_PROBE_OFFSET_DIGITS : MOTION_MILLI_DIGITS, i != GRAV_STRENGTH);
    }
    if (snprintf(out, len, "GRAV %s,%s,%s S%s B%s,%s,%s Q%c", gravity_text[GRAV_X], gravity_text[GRAV_Y],
                 gravity_text[GRAV_Z], gravity_text[GRAV_STRENGTH], gravity_text[GRAV_BIAS_X],
                 gravity_text[GRAV_BIAS_Y], gravity_text[GRAV_BIAS_Z], quarter_digit)
            < 0
        && len > 0) {
        out[0] = '\0';
    }
}
