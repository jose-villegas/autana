/* gravity_figures_probe: chip-sampled reader motion, acceleration rejection and gyro bias learning. */
#include "device_motion_probe.h"
#include "input/motion/gravity.h"
#include "math/text/math_text.h"

#define PROBE_STEPS           100
#define PROBE_TILT_RAD        0.05F
#define PROBE_ERROR_MAX_G     (GRAVITY_TRUST_NONE_G + GRAVITY_TRUST_FULL_G)
#define PROBE_DURATION_S      (GRAVITY_BIAS_SETTLE_S * 1.5F)
/* Figure frames per second of simulated time; each frame's samples arrive as one batch. */
#define PROBE_FRAME_HZ        4
/* Gyro offset on chip X, Y and Z in deg/s: datasheet-sized, inside IMU_GYRO_BIAS_MAX_DPS. */
#define PROBE_BIAS_DPS        {IMU_GYRO_OFFSET_TOL_DPS, -IMU_GYRO_OFFSET_TOL_DPS / 2, IMU_GYRO_OFFSET_TOL_DPS / 2}
/* The rocking about flat, screen up: two slow tilts about device x and y. */
#define PROBE_PITCH_DEG       20.0F
#define PROBE_PITCH_S         7.0F
#define PROBE_ROLL_DEG        20.0F
#define PROBE_ROLL_S          11.0F

#define SCENARIO_FRAME_HZ     10
#define READER_DURATION_S     8.0F
#define READER_TILT_START_S   0.5F
#define READER_TILT_END_S     3.0F
#define READER_PITCH_DEG      70.0F
#define READER_ROLL_END_S     6.0F
#define READER_ROLL_DEG       40.0F
#define READER_FALL_START_S   6.6F
#define READER_FALL_END_S     7.0F
#define MOTION_DURATION_S     6.0F
#define MOTION_PITCH_DEG      30.0F
#define MOTION_ROLL_DEG       20.0F
#define MOTION_ROLL_PERIOD_S  5.0F
#define MOTION_ACCEL_G        0.6F
#define MOTION_ACCEL_PERIOD_S 1.2F
#define MOTION_START_S        1.0F
#define MOTION_FADE_S         0.5F
#define MOTION_END_S          5.0F

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

static probe_pose_t
tilt_pose(float pitch, float roll) {
    const float tip[3][3] = {{1, 0, 0}, {0, cosf(pitch), -sinf(pitch)}, {0, sinf(pitch), cosf(pitch)}};
    const float tilt[3][3] = {{cosf(roll), 0, sinf(roll)}, {0, 1, 0}, {-sinf(roll), 0, cosf(roll)}};
    probe_pose_t p;
    probe_rotate(p.m, tip, tilt);
    return p;
}

/* Device to world at time t: pitch about device x, then roll about device y; world z is up. */
static probe_pose_t
pose(float t) {
    return tilt_pose(PROBE_PITCH_DEG * PROBE_DEG_TO_RAD * sinf(2 * PROBE_PI * t / PROBE_PITCH_S),
                     PROBE_ROLL_DEG * PROBE_DEG_TO_RAD * sinf(2 * PROBE_PI * t / PROBE_ROLL_S));
}

static probe_pose_t
reader_pose(float t) {
    const float roll_time = fminf(READER_ROLL_END_S, fmaxf(READER_TILT_END_S, t)) - READER_TILT_END_S;
    return tilt_pose(READER_PITCH_DEG * PROBE_DEG_TO_RAD * probe_ease(t, READER_TILT_START_S, READER_TILT_END_S),
                     READER_ROLL_DEG * PROBE_DEG_TO_RAD
                         * sinf(2 * PROBE_PI * roll_time / (READER_ROLL_END_S - READER_TILT_END_S)));
}

static void
reader_linear(float t, float out[3]) {
    out[2] = t >= READER_FALL_START_S && t < READER_FALL_END_S ? -1.0F : 0.0F;
}

static probe_pose_t
motion_pose(float t) {
    return tilt_pose(MOTION_PITCH_DEG * PROBE_DEG_TO_RAD,
                     MOTION_ROLL_DEG * PROBE_DEG_TO_RAD * sinf(2 * PROBE_PI * t / MOTION_ROLL_PERIOD_S));
}

static void
motion_linear(float t, float out[3]) {
    const float envelope = probe_ease(t, MOTION_START_S, MOTION_START_S + MOTION_FADE_S)
                           * (1.0F - probe_ease(t, MOTION_END_S - MOTION_FADE_S, MOTION_END_S));
    out[0] = MOTION_ACCEL_G * envelope * sinf(2 * PROBE_PI * t / MOTION_ACCEL_PERIOD_S);
}

static void
print_vec(const char* name, vec3f_t v) {
    printf(",\"%s\":[%s]", name, vec3f_text(v).chars);
}

static void
print_scenario(const char* name, probe_pose_at_t pose_at, probe_linear_at_t linear, float duration) {
    printf(",\"%s\":[", name);
    gravity_t g;
    gravity_reset(&g);
    const float dt = (float)INPUT_MOTION_SAMPLE_US / MATH_US_PER_S;
    const float no_bias[3] = {0, 0, 0};
    const unsigned frames = (unsigned)(duration * SCENARIO_FRAME_HZ);
    unsigned index = 0;
    for (unsigned f = 0; f <= frames; f++) {
        const unsigned end = (unsigned)lroundf(f / (SCENARIO_FRAME_HZ * dt)) + 1;
        input_motion_batch_t batch = {.count = (int)(end - index)};
        for (int i = 0; i < batch.count; i++, index++) {
            batch.samples[i] = probe_motion_sample(pose_at, index * dt, dt, no_bias, linear);
        }
        gravity_update_batch(&g, &batch);
        const float t = (index - 1) * dt;
        const probe_pose_t p = pose_at(t);
        float truth[3];
        probe_to_device(&p, (const float[3]){0, 0, -1}, truth);
        printf("%s{\"t\":%g,", f ? "," : "", (double)t);
        probe_print_pose(&p);
        print_vec("true_down", (vec3f_t){truth[0], truth[1], truth[2]});
        print_vec("down", gravity_down(&g));
        print_vec("linear", gravity_linear(&g));
        printf(",\"strength\":%g,\"free_fall\":%s}", (double)gravity_strength(&g),
               gravity_in_free_fall(&g) ? "true" : "false");
    }
    printf("]");
}

int
main(void) {
    printf("{\"full_g\":%g,\"none_g\":%g,\"tau_s\":%g,\"settle_s\":%g,\"ki\":%g,\"trust\":[",
           (double)GRAVITY_TRUST_FULL_G, (double)GRAVITY_TRUST_NONE_G, (double)GRAVITY_TAU_S,
           (double)GRAVITY_BIAS_SETTLE_S, (double)GRAVITY_BIAS_KI);
    const float full_bias = correction_bias(0.0F);
    for (int i = 0; i <= PROBE_STEPS; i++) {
        const float error = PROBE_ERROR_MAX_G * i / PROBE_STEPS;
        printf("%s[%g,%g]", i == 0 ? "" : ",", (double)error, (double)(correction_bias(error) / full_bias));
    }
    printf("]");
    const float bias[3] = PROBE_BIAS_DPS;
    const vec3i_t screen_bias =
        imu_axes_screen((int)lroundf(bias[0] * IMU_COUNTS_PER_DPS), (int)lroundf(bias[1] * IMU_COUNTS_PER_DPS),
                        (int)lroundf(bias[2] * IMU_COUNTS_PER_DPS));
    print_vec("bias_dps",
              (vec3f_t){(float)screen_bias.x / INPUT_ROTATION_ONE_DPS, (float)screen_bias.y / INPUT_ROTATION_ONE_DPS,
                        (float)screen_bias.z / INPUT_ROTATION_ONE_DPS});
    printf(",\"frames\":[");
    gravity_t g;
    gravity_reset(&g);
    const float dt = (float)INPUT_MOTION_SAMPLE_US / MATH_US_PER_S;
    const unsigned per_frame = (unsigned)lroundf(1.0F / (PROBE_FRAME_HZ * dt));
    const unsigned frames = (unsigned)(PROBE_DURATION_S * PROBE_FRAME_HZ);
    unsigned index = 0;
    for (unsigned f = 0; f <= frames; f++) {
        input_motion_batch_t batch = {.count = f == 0 ? 1 : (int)per_frame};
        for (int i = 0; i < batch.count; i++, index++) {
            batch.samples[i] = probe_motion_sample(pose, index * dt, dt, bias, NULL);
        }
        gravity_update_batch(&g, &batch);
        const float t = (index - 1) * dt;
        const probe_pose_t p = pose(t);
        float down[3];
        probe_to_device(&p, (const float[3]){0, 0, -1}, down);
        printf("%s{\"t\":%g,", f ? "," : "", (double)t);
        probe_print_pose(&p);
        printf(",\"true_down\":[%g,%g,%g]", (double)down[0], (double)down[1], (double)down[2]);
        print_vec("down", gravity_down(&g));
        print_vec("bias", gravity_gyro_bias(&g));
        printf("}");
    }
    printf("]");
    printf(",\"min_direction\":%g,\"free_fall_g\":%g,\"reject_deg\":%g,\"fall_span\":[%g,%g]",
           (double)GRAVITY_MIN_DIRECTION_STRENGTH, (double)GRAVITY_FREE_FALL_G, (double)GRAVITY_REJECT_DEG,
           (double)READER_FALL_START_S, (double)READER_FALL_END_S);
    print_scenario("reader", reader_pose, reader_linear, READER_DURATION_S);
    print_scenario("motion", motion_pose, motion_linear, MOTION_DURATION_S);
    puts("}");
    return 0;
}
