/* gravity_figures_probe: measured correction gain and stationary gyro bias settling. */
#include <math.h>
#include <stdio.h>

#include "input/motion/gravity.h"

#define PROBE_STEPS       100
#define PROBE_TILT_RAD    0.05F
#define PROBE_ERROR_MAX_G (GRAVITY_TRUST_NONE_G + GRAVITY_TRUST_FULL_G)
#define PROBE_DURATION_S  (GRAVITY_BIAS_SETTLE_S * 1.5F)

static float
correction_bias(float error) {
    gravity_t g;
    gravity_reset(&g);
    g.primed = true;
    g.down = (vec3f_t){0.0F, sinf(PROBE_TILT_RAD), -cosf(PROBE_TILT_RAD)};
    const input_motion_t m = {.acceleration = {.z = -(int)((1.0F + error) * INPUT_ACCEL_ONE_G)}};
    gravity_update(&g, &m, INPUT_MOTION_SAMPLE_US);
    return gravity_gyro_bias(&g).x;
}

int
main(void) {
    printf("{\"full_g\":%g,\"none_g\":%g,\"settle_s\":%g,\"offset_dps\":%g,\"trust\":[", (double)GRAVITY_TRUST_FULL_G,
           (double)GRAVITY_TRUST_NONE_G, (double)GRAVITY_BIAS_SETTLE_S, (double)GRAVITY_GYRO_OFFSET_TOL_DPS);
    const float full_bias = correction_bias(0.0F);
    for (int i = 0; i <= PROBE_STEPS; i++) {
        const float error = PROBE_ERROR_MAX_G * i / PROBE_STEPS;
        printf("%s[%g,%g]", i == 0 ? "" : ",", (double)error, (double)(correction_bias(error) / full_bias));
    }
    printf("],\"bias\":[[0,0]");
    gravity_t g;
    gravity_reset(&g);
    input_motion_t m = {.acceleration = {.z = -INPUT_ACCEL_ONE_G}};
    gravity_update(&g, &m, 0);
    m.rotation.x = (int)(GRAVITY_GYRO_OFFSET_TOL_DPS * INPUT_ROTATION_ONE_DPS);
    const unsigned steps = (unsigned)(PROBE_DURATION_S * MATH_US_PER_S / INPUT_MOTION_SAMPLE_US);
    for (unsigned i = 1; i <= steps; i++) {
        gravity_update(&g, &m, INPUT_MOTION_SAMPLE_US);
        printf(",[%g,%g]", (double)i * INPUT_MOTION_SAMPLE_US / MATH_US_PER_S, (double)gravity_gyro_bias(&g).x);
    }
    puts("]}");
    return 0;
}
