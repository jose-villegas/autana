/* motion_axes_figures_probe: the device upright, facing the viewer, turned out and back about each rotation
 * label x, y, z in turn, sampled as the QMI8658 would read it and mapped as input_read_motion() maps it. */
#include "device_motion_probe.h"
#include "input/input.h"
#include "math/scalar/units.h"

/* Each turn goes out by PROBE_TURN_DEG and back over PROBE_TURN_S, with no rate at either end. */
#define PROBE_TURN_DEG 90.0F
#define PROBE_TURN_S   4.0F
#define PROBE_AXES     3
#define PROBE_FRAME_HZ 10

/* Upright, facing the viewer: device x right, y up, z towards the viewer (world -y). */
static const float upright[3][3] = {{1, 0, 0}, {0, 0, -1}, {0, 1, 0}};

/* The device axis behind each rotation label, where imu_axes_screen() sends the chip's rate axes. */
static float label_axes[PROBE_AXES][3];

/* Device to world at time t: upright, turned about the current label's axis in device axes. */
static probe_pose_t
pose(float t) {
    const int label = (int)(t / PROBE_TURN_S) < PROBE_AXES ? (int)(t / PROBE_TURN_S) : PROBE_AXES - 1;
    const float phase = (t - label * PROBE_TURN_S) / PROBE_TURN_S;
    const float angle = PROBE_TURN_DEG * PROBE_DEG_TO_RAD * (1 - cosf(2 * PROBE_PI * phase)) / 2;
    const float* k = label_axes[label];
    const float c = cosf(angle), s = sinf(angle);
    /* Rodrigues: rotation by angle about unit k. */
    const float turn[3][3] = {
        {c + k[0] * k[0] * (1 - c), k[0] * k[1] * (1 - c) - k[2] * s, k[0] * k[2] * (1 - c) + k[1] * s},
        {k[1] * k[0] * (1 - c) + k[2] * s, c + k[1] * k[1] * (1 - c), k[1] * k[2] * (1 - c) - k[0] * s},
        {k[2] * k[0] * (1 - c) - k[1] * s, k[2] * k[1] * (1 - c) + k[0] * s, c + k[2] * k[2] * (1 - c)}};
    probe_pose_t p;
    probe_rotate(p.m, upright, turn);
    return p;
}

int
main(void) {
    probe_label_axes(label_axes);
    printf("{\"turn_deg\":%g,\"turn_s\":%g,\"frames\":[", (double)PROBE_TURN_DEG, (double)PROBE_TURN_S);
    const float dt = (float)INPUT_MOTION_SAMPLE_US / MATH_US_PER_S;
    const int frames = (int)(PROBE_AXES * PROBE_TURN_S * PROBE_FRAME_HZ);
    const float none[3] = {0};
    for (int f = 0; f <= frames; f++) {
        const float t = (float)f / PROBE_FRAME_HZ;
        const probe_pose_t p = pose(t);
        const imu_sample_t chip = probe_chip_sample(pose, t, dt, none);
        const vec3i_t acceleration = imu_axes_screen(chip.ax, chip.ay, chip.az);
        const vec3i_t rotation = imu_axes_screen(chip.gx, chip.gy, chip.gz);
        float rate[3];
        probe_body_rate(pose, t, dt, rate);
        printf("%s{\"t\":%g,", f ? "," : "", (double)t);
        probe_print_pose(&p);
        printf(",\"acceleration\":[%g,%g,%g],\"rotation\":[%g,%g,%g],\"turn_speed\":%d,\"rate\":[%g,%g,%g]}",
               (double)acceleration.x / INPUT_ACCEL_ONE_G, (double)acceleration.y / INPUT_ACCEL_ONE_G,
               (double)acceleration.z / INPUT_ACCEL_ONE_G, (double)rotation.x / INPUT_ROTATION_ONE_DPS,
               (double)rotation.y / INPUT_ROTATION_ONE_DPS, (double)rotation.z / INPUT_ROTATION_ONE_DPS,
               imu_turn_speed((vec3i_t){chip.gx, chip.gy, chip.gz}), (double)(rate[0] / PROBE_DEG_TO_RAD),
               (double)(rate[1] / PROBE_DEG_TO_RAD), (double)(rate[2] / PROBE_DEG_TO_RAD));
    }
    puts("]}");
    return 0;
}
