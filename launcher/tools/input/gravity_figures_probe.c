/* gravity_figures_probe: correction trust weight (via bias gain at a 0.05 rad tilt), and the device
 * rocking with a known gyro bias through gravity_update_batch, sampled as the QMI8658 would read it. */
#include "device_motion_probe.h"
#include "input/motion/gravity.h"
#include "math/text/math_text.h"

#define PROBE_STEPS       100
#define PROBE_TILT_RAD    0.05F
#define PROBE_ERROR_MAX_G (GRAVITY_TRUST_NONE_G + GRAVITY_TRUST_FULL_G)
#define PROBE_DURATION_S  (GRAVITY_BIAS_SETTLE_S * 1.5F)
/* Figure frames per second of simulated time; each frame's samples arrive as one batch. */
#define PROBE_FRAME_HZ    4
/* Gyro offset on chip X, Y and Z in deg/s: datasheet-sized, inside IMU_GYRO_BIAS_MAX_DPS. */
#define PROBE_BIAS_DPS    {IMU_GYRO_OFFSET_TOL_DPS, -IMU_GYRO_OFFSET_TOL_DPS / 2, IMU_GYRO_OFFSET_TOL_DPS / 2}
/* The rocking about flat, screen up: two slow tilts about device x and y. */
#define PROBE_PITCH_DEG   20.0F
#define PROBE_PITCH_S     7.0F
#define PROBE_ROLL_DEG    20.0F
#define PROBE_ROLL_S      11.0F

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

/* Device to world at time t: pitch about device x, then roll about device y; world z is up. */
static probe_pose_t
pose(float t) {
    const float pitch = PROBE_PITCH_DEG * PROBE_DEG_TO_RAD * sinf(2 * PROBE_PI * t / PROBE_PITCH_S);
    const float roll = PROBE_ROLL_DEG * PROBE_DEG_TO_RAD * sinf(2 * PROBE_PI * t / PROBE_ROLL_S);
    const float tip[3][3] = {{1, 0, 0}, {0, cosf(pitch), -sinf(pitch)}, {0, sinf(pitch), cosf(pitch)}};
    const float tilt[3][3] = {{cosf(roll), 0, sinf(roll)}, {0, 1, 0}, {-sinf(roll), 0, cosf(roll)}};
    probe_pose_t p;
    probe_rotate(p.m, tip, tilt);
    return p;
}

/* What the chip reads, offset by the gyro bias, mapped by imu_axes_screen. */
static input_motion_t
sample(float t, float dt) {
    const float bias[3] = PROBE_BIAS_DPS;
    const imu_sample_t chip = probe_chip_sample(pose, t, dt, bias);
    return (input_motion_t){.acceleration = imu_axes_screen(chip.ax, chip.ay, chip.az),
                            .rotation = imu_axes_screen(chip.gx, chip.gy, chip.gz)};
}

static void
print_vec(const char* name, vec3f_t v) {
    printf(",\"%s\":[%s]", name, vec3f_text(v).chars);
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
            batch.samples[i] = sample(index * dt, dt);
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
    puts("]}");
    return 0;
}
