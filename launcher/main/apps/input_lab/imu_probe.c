#include "apps/input_lab/imu_probe.h"

#include <ctype.h>
#include <math.h>
#include "math/linear/vec_convert.h"
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
    const affine2x_t transform = ui_transform_quarter_turn(quarter, 0, 0);
    const vec2i_t mapped_pixel = affine2x_map_pixel(&transform, (vec2i_t){x, y});
    return (vec3i_t){mapped_pixel.x, mapped_pixel.y, 0};
}

/* Six-position offset and sensitivity: ST AN4508.
 * Accel offset is calib.offset in Q12 counts; imu_probe_mg converts it.
 * Sensitivity is ppm (unity = IMU_PROBE_SCALE_ONE), gyro offset mdps. */
imu_probe_fit_t
imu_probe_fit(const imu_rest_t poses[IMU_PROBE_POSE_COUNT], int quarter) {
    int32_t up[3] = {0};
    int32_t down[3] = {0};
    float gyro[3] = {0};
    int n = 0;
    for (int pose = 0; pose < IMU_PROBE_POSE_COUNT; pose++) {
        if (poses[pose].n == 0) {
            continue;
        }
        const input_motion_t mean = imu_rest_mean(&poses[pose]);
        const int32_t a[] = {mean.acceleration.x, mean.acceleration.y, mean.acceleration.z};
        const vec3i_t direction = imu_probe_pose_direction(pose, quarter);
        const int d[] = {direction.x, direction.y, direction.z};
        for (int axis = 0; axis < 3; axis++) {
            if (d[axis] > 0) {
                up[axis] = a[axis];
            }
            if (d[axis] < 0) {
                down[axis] = a[axis];
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
    *round = (imu_probe_round_t){.step = IMU_PROBE_POSES, .quarter = quarter};
}

static bool
gravity_aligned(const imu_probe_round_t* round, input_motion_t sample) {
    const int pose = round->pose == IMU_PROBE_POSE_COUNT ? IMU_PROBE_BOTTOM_DOWN : round->pose;
    const vec3i_t direction = imu_probe_pose_direction(pose, round->quarter);
    if (round->step == IMU_PROBE_UPRIGHT) {
        const vec3f_t acceleration = vec3f_from_vec3i(sample.acceleration);
        const float magnitude = sqrtf(vec3f_dot(acceleration, acceleration));
        return magnitude > 0.0F
               && (float)vec3i_dot(direction, sample.acceleration) >= IMU_PROBE_UPRIGHT_MIN * magnitude;
    }
    return vec3i_dot(direction, sample.acceleration) >= IMU_PROBE_GRAVITY_MIN;
}

bool
imu_probe_update(imu_probe_round_t* round, const input_motion_t* sample, uint32_t dt_ms) {
    if (round->step != IMU_PROBE_POSES || dt_ms == 0) {
        return false;
    }
    const imu_rest_stage_t stage =
        imu_rest_step(&round->rest, sample, dt_ms * MATH_US_PER_MS, sample != NULL && gravity_aligned(round, *sample));
    if (stage != IMU_REST_DONE) {
        return false;
    }
    round->poses[round->pose++] = round->rest;
    if (round->pose == IMU_PROBE_POSE_COUNT) {
        round->fit = imu_probe_fit(round->poses, round->quarter);
        round->step = IMU_PROBE_UPRIGHT;
    }
    return true;
}

static bool
turning(input_motion_t sample) {
    const vec3f_t rotation = vec3f_from_vec3i(sample.rotation);
    return vec3f_dot(rotation, rotation) > (float)IMU_PROBE_TURN_THRESHOLD * IMU_PROBE_TURN_THRESHOLD;
}

static void
turn_upright(imu_probe_round_t* round, const input_motion_t* sample, uint32_t dt_ms) {
    if (sample == NULL || dt_ms > IMU_REST_MAX_GAP_MS || !gravity_aligned(round, *sample) || turning(*sample)) {
        round->turn_ms = 0;
        return;
    }
    round->turn_ms += dt_ms;
    if (round->turn_ms >= IMU_REST_SETTLE_MS) {
        round->turn_ms = 0;
        round->step = IMU_PROBE_TURN;
    }
}

static bool
turn_trace(imu_probe_round_t* round, const input_motion_t* sample, vec3f_t down, uint32_t dt_ms) {
    const uint32_t remaining_ms = IMU_PROBE_TURN_WINDOW_MS - round->turn_ms;
    const bool within_window = dt_ms <= remaining_ms;
    round->turn_ms += dt_ms < remaining_ms ? dt_ms : remaining_ms;
    if (round->turn_ms == IMU_PROBE_TURN_WINDOW_MS) {
        round->step = IMU_PROBE_DONE;
    }
    if (!within_window || sample == NULL
        || round->turn_ms / IMU_PROBE_TURN_PERIOD_MS == round->trace_ms / IMU_PROBE_TURN_PERIOD_MS) {
        return false;
    }
    round->trace_ms = round->turn_ms;
    round->trace_down = down;
    round->trace_count++;
    return true;
}

bool
imu_probe_turn_update(imu_probe_round_t* round, const input_motion_t* sample, vec3f_t down, uint32_t dt_ms) {
    if (dt_ms == 0 || round->step == IMU_PROBE_IDLE || round->step == IMU_PROBE_POSES
        || round->step == IMU_PROBE_DONE) {
        return false;
    }
    if (round->step == IMU_PROBE_UPRIGHT) {
        turn_upright(round, sample, dt_ms);
        return false;
    }
    if (round->step == IMU_PROBE_TRACE) {
        return turn_trace(round, sample, down, dt_ms);
    }
    if (sample == NULL || !turning(*sample)) {
        return false;
    }
    round->step = IMU_PROBE_TRACE;
    round->trace_down = down;
    round->trace_count = 1;
    return true;
}
