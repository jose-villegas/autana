/* motion_reader: shared on-demand motion readers. */
#include "input/motion/motion_reader.h"

#include <math.h>
#include "build/build_variant.h"
#include "core/timing.h"
#include "input/input.h"
#include "input/input_shell.h"
#include "input/motion/gravity.h"
#include "input/motion/imu.h"
#include "input/motion/imu_rest.h"
#include "input/motion/shake.h"
#include "math/linear/vec_convert.h"
#include "math/scalar/mathf_round.h"
#if CONFIG_LAUNCHER_DEVELOPMENT
#include "core/tune.h"
#include "esp_log.h"
#define MOTION_LOG_MAX_MS (60 * MATH_MS_PER_S)
#define MOTION_MG_PER_G   1000
TUNE_OWNER(motion);
TUNE(motion, log_ms, 0, 0, MOTION_LOG_MAX_MS);
static int64_t last_log_us;
#endif

static gravity_t gravity = {.down = {0, 0, -1}};
static shake_t shake;
static imu_rest_t rest;
static int64_t rotation[3];
static uint32_t samples;
static vec3f_t down = {0, 1, 0};
static int64_t updated_us = INT64_MIN;
static uint32_t updated_sequence;

void
motion_reader_reset(void) {
    gravity_reset(&gravity);
    imu_rest_reset(&rest);
    samples = 0;
    for (int axis = 0; axis < 3; axis++) {
        rotation[axis] = 0;
    }
    shake_reset(&shake);
    down = (vec3f_t){0, 1, 0};
    updated_us = INT64_MIN;
    updated_sequence = 0;
}

static void
update_without_sample(void) {
    imu_rest_reset(&rest);
    samples = 0;
    for (int axis = 0; axis < 3; axis++) {
        rotation[axis] = 0;
    }
    if (!imu_ready()) {
        down = (vec3f_t){0, 1, 0};
        shake_reset(&shake);
    } else if (updated_us == INT64_MIN) {
        down = (vec3f_t){0};
        shake_reset(&shake);
    }
}

static int64_t
sample_elapsed_us(int64_t now_us) {
    return updated_us != INT64_MIN && now_us > updated_us ? now_us - updated_us : 0;
}

static void
update_rest_learning(const input_motion_t* sample, int64_t elapsed) {
    const vec3f_t a = vec3f_scale(vec3f_from_vec3i(sample->acceleration), 1.0F / INPUT_ACCEL_ONE_G);
    const bool gate = elapsed <= GRAVITY_MAX_DT_US && fabsf(sqrtf(vec3f_dot(a, a)) - 1.0F) <= GRAVITY_TRUST_FULL_G;
    const uint32_t rest_dt_us = elapsed > UINT32_MAX ? UINT32_MAX : (uint32_t)elapsed;
    const imu_rest_stage_t stage = imu_rest_step(&rest, sample, rest_dt_us, gate);
    if (stage == IMU_REST_MEASURING || stage == IMU_REST_DONE) {
        const int32_t r[] = {sample->rotation.x, sample->rotation.y, sample->rotation.z};
        for (int axis = 0; axis < 3; axis++) {
            rotation[axis] += r[axis];
        }
        samples++;
        if (stage == IMU_REST_DONE) {
            const float n = (float)samples;
            const vec3i_t residual = {mathf_round_i32((float)rotation[0] / n), mathf_round_i32((float)rotation[1] / n),
                                      mathf_round_i32((float)rotation[2] / n)};
            if (input_learn_gyro_offset(residual)) {
                gravity_clear_gyro_bias(&gravity);
            }
        }
    }
    if (rest_dt_us != 0 && stage != IMU_REST_MEASURING) {
        for (int axis = 0; axis < 3; axis++) {
            rotation[axis] = 0;
        }
        samples = 0;
    }
}

static void
update(void) {
    input_motion_t sample;
    uint32_t sequence;
    if (!input_read_motion_sequence(&sample, &sequence)) {
        update_without_sample();
        return;
    }
    if (updated_us != INT64_MIN && sequence == updated_sequence) {
        return;
    }
    const int64_t now_us = timing_now_us();
    const int64_t elapsed = sample_elapsed_us(now_us);
    const uint32_t dt_us = elapsed > GRAVITY_MAX_DT_US ? GRAVITY_MAX_DT_US : (uint32_t)elapsed;
    updated_us = now_us;
    updated_sequence = sequence;
    gravity_update(&gravity, &sample, dt_us);
    update_rest_learning(&sample, elapsed);
    shake_update(&shake, gravity_linear(&gravity), dt_us);
    down = gravity_in_free_fall(&gravity) ? (vec3f_t){0} : gravity_down(&gravity);
#if CONFIG_LAUNCHER_DEVELOPMENT
    if (log_ms > 0 && now_us - last_log_us >= (int64_t)log_ms * MATH_US_PER_MS) {
        ESP_LOGI("motion", "shake_smooth_mg %d", (int)lroundf(shake.smooth_g * MOTION_MG_PER_G));
        last_log_us = now_us;
    }
#endif
}

vec3f_t
input_gravity(void) {
    update();
    return down;
}

float
input_shake(void) {
    update();
    return shake_level(&shake);
}
