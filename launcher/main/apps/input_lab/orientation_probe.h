/* orientation_probe: guided script for replaying shell quarter decisions. */
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "apps/input_lab/imu_probe.h"
#include "math/scalar/units.h"

#define ORIENTATION_PROBE_STEP_MS          10000U
#define ORIENTATION_PROBE_READING_TILT_DEG 45
#define ORIENTATION_PROBE_PROMPT_SIZE      96

typedef enum {
    ORIENTATION_PROBE_READ_0,
    ORIENTATION_PROBE_READ_1,
    ORIENTATION_PROBE_READ_2,
    ORIENTATION_PROBE_READ_3,
    ORIENTATION_PROBE_READ_4,
    ORIENTATION_PROBE_FLAT,
    ORIENTATION_PROBE_PICKUP,
    ORIENTATION_PROBE_WALK,
    ORIENTATION_PROBE_SHAKE,
    ORIENTATION_PROBE_OVERHEAD,
    ORIENTATION_PROBE_DONE
} orientation_probe_step_t;

#define ORIENTATION_PROBE_ROUND_MS        (ORIENTATION_PROBE_DONE * ORIENTATION_PROBE_STEP_MS)
/* Allow as much ready dwell as timed recording before the transcript fills. */
#define ORIENTATION_PROBE_READY_BUDGET_MS ORIENTATION_PROBE_ROUND_MS
/* Samples plus step and ready-gate markers, including round and completion. */
#define ORIENTATION_PROBE_RECORD_LINES                                                                                 \
    (((ORIENTATION_PROBE_ROUND_MS + ORIENTATION_PROBE_READY_BUDGET_MS) * MATH_US_PER_MS + INPUT_MOTION_SAMPLE_US - 1)  \
         / INPUT_MOTION_SAMPLE_US                                                                                      \
     + (3 * ORIENTATION_PROBE_DONE) + 2)
/* Counts retain offset-calibrated acceleration and raw gyro precision. */
#define ORIENTATION_PROBE_SAMPLE_FORMAT    "orientation %d %lld %d %d %d %d %d %d"
#define ORIENTATION_PROBE_ACCEL_MAX_COUNTS (-INT16_MIN + IMU_CALIB_OFFSET_MAX)
#define ORIENTATION_PROBE_GYRO_MAX_COUNTS  (-INT16_MIN)
#define ORIENTATION_PROBE_ACCEL_WIDTH      (1 + IMU_PROBE_DECIMAL_DIGITS(ORIENTATION_PROBE_ACCEL_MAX_COUNTS))
#define ORIENTATION_PROBE_GYRO_WIDTH       (1 + IMU_PROBE_DECIMAL_DIGITS(ORIENTATION_PROBE_GYRO_MAX_COUNTS))
#define ORIENTATION_PROBE_SAMPLE_FIELDS    (2 + 3 + 3)
#define ORIENTATION_PROBE_SAMPLE_LABEL_SIZE                                                                            \
    (sizeof(ORIENTATION_PROBE_SAMPLE_FORMAT) - 1 - (ORIENTATION_PROBE_SAMPLE_FIELDS * (sizeof("%d") - 1))              \
     - (sizeof("%lld") - sizeof("%d")))
#define ORIENTATION_PROBE_SAMPLE_SIZE                                                                                  \
    (ORIENTATION_PROBE_SAMPLE_LABEL_SIZE + IMU_PROBE_DECIMAL_DIGITS(ORIENTATION_PROBE_DONE)                            \
     + IMU_PROBE_DECIMAL_DIGITS(ORIENTATION_PROBE_ROUND_MS * MATH_US_PER_MS) + (3 * ORIENTATION_PROBE_ACCEL_WIDTH)     \
     + (3 * ORIENTATION_PROBE_GYRO_WIDTH))
const char* orientation_probe_action(orientation_probe_step_t step);
int orientation_probe_bearing(int quarter);
int orientation_probe_target(orientation_probe_step_t step);
void orientation_probe_prompt(orientation_probe_step_t step, char* text, size_t size);
