#include "apps/input_lab/imu_probe.h"

#include <ctype.h>
#include <math.h>
#include "math/scalar/mathf_round.h"

#include "ui/layout/ui_transform.h"

const char*
imu_probe_pose_name(int pose) {
    static const char* const names[IMU_PROBE_POSE_COUNT] = {"face_up",    "face_down", "left_down",
                                                            "right_down", "top_down",  "bottom_down"};
    return names[pose];
}

void
imu_probe_pose_label(int pose, char* out, size_t size) {
    const char* name = imu_probe_pose_name(pose);
    if (size == 0) {
        return;
    }
    size_t i = 0;
    for (; name[i] && i + 1 < size; i++) {
        out[i] = name[i] == '_' ? ' ' : (char)toupper((unsigned char)name[i]);
    }
    out[i] = '\0';
}

vec3i_t
imu_probe_pose_direction(int pose, int quarter) {
    if (pose == IMU_PROBE_FACE_UP || pose == IMU_PROBE_FACE_DOWN) {
        return (vec3i_t){0, 0, pose == IMU_PROBE_FACE_UP ? -1 : 1};
    }
    const int x = pose == IMU_PROBE_LEFT_DOWN ? -1 : pose == IMU_PROBE_RIGHT_DOWN ? 1 : 0;
    const int y = pose == IMU_PROBE_TOP_DOWN ? -1 : pose == IMU_PROBE_BOTTOM_DOWN ? 1 : 0;
    const vec2i_t mapped_pixel = affine2x_map_pixel(ui_transform_quarter_turn(quarter, 0, 0), (vec2i_t){x, y});
    return (vec3i_t){mapped_pixel.x, mapped_pixel.y, 0};
}

void
imu_probe_average_add(imu_probe_average_t* average, input_motion_t sample) {
    const int a[] = {sample.acceleration.x, sample.acceleration.y, sample.acceleration.z};
    const int r[] = {sample.rotation.x, sample.rotation.y, sample.rotation.z};
    for (int axis = 0; axis < 3; axis++) {
        average->acceleration[axis] += a[axis];
        average->rotation[axis] += r[axis];
    }
    average->n++;
}

input_motion_t
imu_probe_average_read(const imu_probe_average_t* average) {
    if (average->n == 0) {
        return (input_motion_t){0};
    }
    int a[3];
    int r[3];
    for (int axis = 0; axis < 3; axis++) {
        a[axis] = (int)mathf_round_i32((float)average->acceleration[axis] / (float)average->n);
        r[axis] = (int)mathf_round_i32((float)average->rotation[axis] / (float)average->n);
    }
    return (input_motion_t){.acceleration = {a[0], a[1], a[2]}, .rotation = {r[0], r[1], r[2]}};
}

/* Six-position offset and sensitivity: docs/Citations.md#67.
 * Accel offset is calib.offset in Q12 counts; imu_probe_mg converts it.
 * Sensitivity is ppm (unity = IMU_PROBE_SCALE_ONE), gyro offset mdps. */
imu_probe_fit_t
imu_probe_fit(const imu_probe_average_t poses[IMU_PROBE_POSE_COUNT], int quarter) {
    int32_t up[3] = {0};
    int32_t down[3] = {0};
    float gyro[3] = {0};
    int n = 0;
    for (int pose = 0; pose < IMU_PROBE_POSE_COUNT; pose++) {
        if (poses[pose].n == 0) {
            continue;
        }
        const vec3i_t direction = imu_probe_pose_direction(pose, quarter);
        const int d[] = {direction.x, direction.y, direction.z};
        for (int axis = 0; axis < 3; axis++) {
            const int32_t mean = mathf_round_i32((float)poses[pose].acceleration[axis] / (float)poses[pose].n);
            if (d[axis] > 0) {
                up[axis] = mean;
            }
            if (d[axis] < 0) {
                down[axis] = mean;
            }
            gyro[axis] += (float)poses[pose].rotation[axis];
        }
        n += poses[pose].n;
    }
    const imu_calib_fit_t fit = imu_calib_fit((vec3i_t){up[0], up[1], up[2]}, (vec3i_t){down[0], down[1], down[2]});
    int rate[3];
    for (int axis = 0; axis < 3; axis++) {
        rate[axis] =
            n > 0 ? (int)mathf_round_i32(gyro[axis] * IMU_PROBE_MILLI / ((float)n * INPUT_ROTATION_ONE_DPS)) : 0;
    }
    return (imu_probe_fit_t){.scale_ppm = fit.scale_ppm, .gyro_mdps = {rate[0], rate[1], rate[2]}, .calib = fit.calib};
}

vec3i_t
imu_probe_saved_offset(vec3i_t stored, const imu_probe_fit_t* fit) {
    return vec3i_add(stored, fit->calib.offset);
}

vec3i_t
imu_probe_mg(vec3i_t q12) {
    const int32_t counts[] = {q12.x, q12.y, q12.z};
    int offset[3];
    for (int axis = 0; axis < 3; axis++) {
        offset[axis] = (int)mathf_round_i32((float)counts[axis] * IMU_PROBE_MILLI / INPUT_ACCEL_ONE_G);
    }
    return (vec3i_t){offset[0], offset[1], offset[2]};
}

void
imu_probe_start(imu_probe_round_t* round, int quarter) {
    *round = (imu_probe_round_t){.active = true, .quarter = quarter};
}

static bool
still(imu_probe_round_t* round, input_motion_t sample, uint32_t dt_ms) {
    const int a[] = {sample.acceleration.x, sample.acceleration.y, sample.acceleration.z};
    const int r[] = {sample.rotation.x, sample.rotation.y, sample.rotation.z};
    const float weight = (float)dt_ms / (IMU_PROBE_RECENT_MS + (float)dt_ms);
    bool stable = round->primed;
    for (int axis = 0; axis < 3; axis++) {
        if (!round->primed) {
            round->recent_accel[axis] = (float)a[axis];
            round->recent_gyro[axis] = (float)r[axis];
        }
        stable = stable && fabsf((float)a[axis] - round->recent_accel[axis]) < IMU_PROBE_ACCEL_DEPARTURE
                 && fabsf((float)r[axis] - round->recent_gyro[axis]) < IMU_PROBE_GYRO_DEPARTURE;
        round->recent_accel[axis] += weight * ((float)a[axis] - round->recent_accel[axis]);
        round->recent_gyro[axis] += weight * ((float)r[axis] - round->recent_gyro[axis]);
    }
    round->primed = true;
    const vec3i_t direction = imu_probe_pose_direction(round->pose, round->quarter);
    return stable && vec3i_dot(direction, sample.acceleration) >= IMU_PROBE_GRAVITY_MIN;
}

bool
imu_probe_update(imu_probe_round_t* round, const input_motion_t* sample, uint32_t dt_ms) {
    if (!round->active || dt_ms == 0) {
        return false;
    }
    if (sample == NULL || dt_ms > IMU_PROBE_MAX_GAP_MS) {
        round->primed = false;
    }
    if (sample == NULL || !still(round, *sample, dt_ms)) {
        round->settle_ms = 0;
        round->window_ms = 0;
        round->window = (imu_probe_average_t){0};
        return false;
    }
    if (round->settle_ms < IMU_PROBE_SETTLE_MS) {
        round->settle_ms += dt_ms;
        return false;
    }
    imu_probe_average_add(&round->window, *sample);
    round->window_ms += dt_ms;
    if (round->window_ms < IMU_PROBE_WINDOW_MS) {
        return false;
    }
    round->poses[round->pose++] = round->window;
    round->primed = false;
    round->settle_ms = round->window_ms = 0;
    round->window = (imu_probe_average_t){0};
    if (round->pose == IMU_PROBE_POSE_COUNT) {
        round->active = false;
        round->fit = imu_probe_fit(round->poses, round->quarter);
    }
    return true;
}
