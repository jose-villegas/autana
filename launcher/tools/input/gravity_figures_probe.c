/* gravity_figures_probe: correction trust weight (via bias gain at a 0.05 rad tilt), and the device
 * rocking with a known gyro bias through gravity_update_batch, sampled as the QMI8658 would read it. */
#include <math.h>
#include <stdio.h>

#include "board/board_geometry.h"
#include "input/motion/gravity.h"
#include "input/motion/imu_sample.h"

#define PROBE_STEPS       100
#define PROBE_TILT_RAD    0.05F
#define PROBE_ERROR_MAX_G (GRAVITY_TRUST_NONE_G + GRAVITY_TRUST_FULL_G)
#define PROBE_DURATION_S  (GRAVITY_BIAS_SETTLE_S * 1.5F)
/* Figure frames per second of simulated time; each frame's samples arrive as one batch. */
#define PROBE_FRAME_HZ    4
/* Gyro offset on chip X, Y and Z in deg/s: datasheet-sized, inside GRAVITY_BIAS_MAX_DPS. */
#define PROBE_BIAS_DPS    {GRAVITY_GYRO_OFFSET_TOL_DPS, -GRAVITY_GYRO_OFFSET_TOL_DPS / 2, GRAVITY_GYRO_OFFSET_TOL_DPS / 2}
/* The rocking about flat, screen up: two slow tilts about device x and y. */
#define PROBE_PITCH_DEG   20.0F
#define PROBE_PITCH_S     7.0F
#define PROBE_ROLL_DEG    20.0F
#define PROBE_ROLL_S      11.0F
#define PROBE_DEG_TO_RAD  (3.14159265F / 180.0F)

typedef struct {
    float m[3][3]; /* columns are device x, y, z in world axes; world z is up */
} probe_pose_t;

static const float chip_axes[3][3] = {BOARD_IMU_AXIS_X, BOARD_IMU_AXIS_Y, BOARD_IMU_AXIS_Z};

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

static void
rotate(float out[3][3], const float a[3][3], const float b[3][3]) {
    for (int r = 0; r < 3; r++) {
        for (int c = 0; c < 3; c++) {
            out[r][c] = a[r][0] * b[0][c] + a[r][1] * b[1][c] + a[r][2] * b[2][c];
        }
    }
}

/* Device to world at time t: pitch about device x, then roll about device y; world z is up. */
static probe_pose_t
pose(float t) {
    const float pitch = PROBE_PITCH_DEG * PROBE_DEG_TO_RAD * sinf(2 * 3.14159265F * t / PROBE_PITCH_S);
    const float roll = PROBE_ROLL_DEG * PROBE_DEG_TO_RAD * sinf(2 * 3.14159265F * t / PROBE_ROLL_S);
    const float tip[3][3] = {{1, 0, 0}, {0, cosf(pitch), -sinf(pitch)}, {0, sinf(pitch), cosf(pitch)}};
    const float tilt[3][3] = {{cosf(roll), 0, sinf(roll)}, {0, 1, 0}, {-sinf(roll), 0, cosf(roll)}};
    probe_pose_t p;
    rotate(p.m, tip, tilt);
    return p;
}

/* World vector w in device axes. */
static void
to_device(const probe_pose_t* p, const float w[3], float d[3]) {
    for (int i = 0; i < 3; i++) {
        d[i] = p->m[0][i] * w[0] + p->m[1][i] * w[1] + p->m[2][i] * w[2];
    }
}

static float
dot(const float a[3], const float b[3]) {
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

/* What the chip reads: specific force (up, 1 g at rest) and body rate plus offset, mapped by imu_axes_screen. */
static input_motion_t
sample(float t, float dt) {
    const probe_pose_t now = pose(t), next = pose(t + dt);
    float up[3], rate[3];
    to_device(&now, (const float[3]){0, 0, 1}, up);
    /* Body rate from R^T R': the skew part over dt, in device axes. */
    float rel[3][3];
    for (int r = 0; r < 3; r++) {
        for (int c = 0; c < 3; c++) {
            rel[r][c] = now.m[0][r] * next.m[0][c] + now.m[1][r] * next.m[1][c] + now.m[2][r] * next.m[2][c];
        }
    }
    rate[0] = (rel[2][1] - rel[1][2]) / (2 * dt);
    rate[1] = (rel[0][2] - rel[2][0]) / (2 * dt);
    rate[2] = (rel[1][0] - rel[0][1]) / (2 * dt);
    const float bias[3] = PROBE_BIAS_DPS;
    int accel[3], gyro[3];
    for (int i = 0; i < 3; i++) {
        accel[i] = (int)lroundf(dot(up, chip_axes[i]) * IMU_COUNTS_PER_G);
        gyro[i] = (int)lroundf((dot(rate, chip_axes[i]) / PROBE_DEG_TO_RAD + bias[i]) * IMU_COUNTS_PER_DPS);
    }
    return (input_motion_t){.acceleration = imu_axes_screen(accel[0], accel[1], accel[2]),
                            .rotation = imu_axes_screen(gyro[0], gyro[1], gyro[2])};
}

static void
print_vec(const char* name, vec3f_t v) {
    printf(",\"%s\":[%g,%g,%g]", name, (double)v.x, (double)v.y, (double)v.z);
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
        to_device(&p, (const float[3]){0, 0, -1}, down);
        printf("%s{\"t\":%g,\"pose\":[", f ? "," : "", (double)t);
        for (int r = 0; r < 3; r++) {
            printf("%s[%g,%g,%g]", r ? "," : "", (double)p.m[r][0], (double)p.m[r][1], (double)p.m[r][2]);
        }
        printf("],\"true_down\":[%g,%g,%g]", (double)down[0], (double)down[1], (double)down[2]);
        print_vec("down", gravity_down(&g));
        print_vec("bias", gravity_gyro_bias(&g));
        printf("}");
    }
    puts("]}");
    return 0;
}
