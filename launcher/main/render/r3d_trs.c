#include "render/r3d_trs.h"

#include <math.h>

#pragma GCC diagnostic error "-Wdouble-promotion"

#define TURN_RADIANS 6.28318530718F
#define HALF_TURN    3.14159265359F

static fix3_unit_t
to_units(float value) {
    return (fix3_unit_t)lroundf(value * (float)FIX3_ONE);
}

/* fix3 negates each angle before taking its sine. */
static fix3_unit_t
angle_to_units(float radians) {
    return to_units(-radians / TURN_RADIANS);
}

static fix3_vec4_t
euler_units(float ax, float ay, float az) {
    return (fix3_vec4_t){angle_to_units(ax), angle_to_units(ay), angle_to_units(az), 0};
}

/* The largest gap, in FIX3_ONE units, between fix3's matrix for these
 * angles and the true one. */
static float
matrix_error(fix3_vec4_t units, const float exact[3][3]) {
    fix3_mat4_t m;
    fix3_rotation_matrix(units.x, units.y, units.z, m);
    float worst = 0.0F;
    for (int r = 0; r < 3; r++) {
        for (int c = 0; c < 3; c++) {
            worst = fmaxf(worst, fabsf((float)m[r][c] - (exact[r][c] * (float)FIX3_ONE)));
        }
    }
    return worst;
}

/* fix3's matrix M (M(row, column), acting on column vectors) has
 * M(1,2) = -sin(x'), M(1,0) : M(1,1) = sin(z') : cos(z') and M(0,2) : M(2,2) =
 * sin(y') : cos(y'), a primed angle being the negated one. The quaternion's
 * matrix gives those entries back. A rotation has two such triples, and
 * fix3's integer sines and truncated products round each differently,
 * so the one whose matrix lands nearer the true rotation is taken. */
static fix3_vec4_t
rotation_units(const float q[4]) {
    const float x = q[0];
    const float y = q[1];
    const float z = q[2];
    const float w = q[3];
    const float exact[3][3] = {
        {1.0F - (2.0F * ((y * y) + (z * z))), 2.0F * ((x * y) - (z * w)), 2.0F * ((x * z) + (y * w))},
        {2.0F * ((x * y) + (z * w)), 1.0F - (2.0F * ((x * x) + (z * z))), 2.0F * ((y * z) - (x * w))},
        {2.0F * ((x * z) - (y * w)), 2.0F * ((y * z) + (x * w)), 1.0F - (2.0F * ((x * x) + (y * y)))},
    };
    const float ax = asinf(fminf(1.0F, fmaxf(-1.0F, -exact[1][2])));
    const float ay = atan2f(exact[0][2], exact[2][2]);
    const float az = atan2f(exact[1][0], exact[1][1]);
    const fix3_vec4_t principal = euler_units(ax, ay, az);
    const fix3_vec4_t other = euler_units(HALF_TURN - ax, ay + HALF_TURN, az + HALF_TURN);
    return matrix_error(other, exact) < matrix_error(principal, exact) ? other : principal;
}

fix3_transform_t
r3d_trs_to_transform(const float translation[3], const float rotation[4], const float scale[3]) {
    fix3_transform_t t;
    t.translation = (fix3_vec4_t){to_units(translation[0]), to_units(translation[1]), to_units(translation[2]), 0};
    t.scale = (fix3_vec4_t){to_units(scale[0]), to_units(scale[1]), to_units(scale[2]), 0};
    t.rotation = rotation_units(rotation);
    return t;
}
