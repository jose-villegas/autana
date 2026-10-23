/* shake_probe: timed capture protocol and sample-rate linear acceleration candidates. */
#pragma once

#include "apps/input_lab/imu_probe.h"
#include "apps/input_lab/motion_feedback.h"
#include "input/input.h"
#include "input/motion/imu_calib.h"
#include "input/motion/shake.h"
#include "math/linear/vec3f.h"
#include "math/scalar/units.h"

#define SHAKE_PROBE_STEP_MS 10000U

typedef enum {
    SHAKE_PROBE_IDLE,
    SHAKE_PROBE_STILL,
    SHAKE_PROBE_HELD,
    SHAKE_PROBE_GENTLE,
    SHAKE_PROBE_HARD,
    SHAKE_PROBE_WALK,
    SHAKE_PROBE_TURN,
    SHAKE_PROBE_FLIP,
    SHAKE_PROBE_TAP,
    SHAKE_PROBE_DONE
} shake_probe_step_t;

#define SHAKE_PROBE_STEP_COUNT (SHAKE_PROBE_DONE - SHAKE_PROBE_STILL)
#define SHAKE_PROBE_ROUND_MS   (SHAKE_PROBE_STEP_COUNT * SHAKE_PROBE_STEP_MS)
/* One line per sample plus the header and step edges; ceiling includes the last partial interval. */
#define SHAKE_PROBE_RECORD_LINES                                                                                       \
    (((SHAKE_PROBE_ROUND_MS * MATH_US_PER_MS + INPUT_MOTION_SAMPLE_US - 1) / INPUT_MOTION_SAMPLE_US)                   \
     + SHAKE_PROBE_STEP_COUNT + 2)

typedef struct {
    shake_probe_step_t step;
    uint32_t ms, flash_ms;
} shake_probe_round_t;

/* SIZE is the instantaneous magnitude in mg; the reader state uses g. */
typedef struct {
    float size;
    shake_t shake;
} shake_probe_levels_t;

void shake_probe_start(shake_probe_round_t* round);
bool shake_probe_running(const shake_probe_round_t* round);
bool shake_probe_advance(shake_probe_round_t* round, uint32_t dt_ms);
const char* shake_probe_prompt(shake_probe_step_t step);
void shake_probe_feed(shake_probe_levels_t* levels, vec3f_t linear_g);

/* Screen-axis negation and the saved calibration can extend the signed sensor range. */
#define SHAKE_PROBE_ACCEL_MAX_MG                                                                                       \
    (((-INT16_MIN + IMU_CALIB_OFFSET_MAX) * IMU_PROBE_MILLI + INPUT_ACCEL_ONE_G - 1) / INPUT_ACCEL_ONE_G)
#define SHAKE_PROBE_LINEAR_MAX_MG    (2 * SHAKE_PROBE_ACCEL_MAX_MG)
/* The L1 bound covers a vector magnitude. */
#define SHAKE_PROBE_CANDIDATE_MAX_MG (3 * SHAKE_PROBE_LINEAR_MAX_MG)
#define SHAKE_PROBE_DECIMAL_DIGITS(value)                                                                              \
    (1 + ((value) >= 10U) + ((value) >= 100U) + ((value) >= 1000U) + ((value) >= 10000U) + ((value) >= 100000U)        \
     + ((value) >= 1000000U) + ((value) >= 10000000U) + ((value) >= 100000000U) + ((value) >= 1000000000U))
#define SHAKE_PROBE_ACCEL_WIDTH     (1 + SHAKE_PROBE_DECIMAL_DIGITS(SHAKE_PROBE_ACCEL_MAX_MG))
#define SHAKE_PROBE_LINEAR_WIDTH    (1 + SHAKE_PROBE_DECIMAL_DIGITS(SHAKE_PROBE_LINEAR_MAX_MG))
#define SHAKE_PROBE_CANDIDATE_WIDTH (1 + SHAKE_PROBE_DECIMAL_DIGITS(SHAKE_PROBE_CANDIDATE_MAX_MG))
#define SHAKE_PROBE_SAMPLE_FORMAT   "shake %d %u acc %d,%d,%d lin %d,%d,%d size %d smooth %d old %d"
#define SHAKE_PROBE_SAMPLE_FIELDS   (2 + (2 * 3) + 2 + 1)
#define SHAKE_PROBE_SAMPLE_LABEL_SIZE                                                                                  \
    (sizeof(SHAKE_PROBE_SAMPLE_FORMAT) - 1 - (SHAKE_PROBE_SAMPLE_FIELDS * (sizeof("%d") - 1)))
#define SHAKE_PROBE_SAMPLE_SIZE                                                                                        \
    (SHAKE_PROBE_SAMPLE_LABEL_SIZE + SHAKE_PROBE_DECIMAL_DIGITS(SHAKE_PROBE_DONE)                                      \
     + SHAKE_PROBE_DECIMAL_DIGITS(SHAKE_PROBE_ROUND_MS) + (3 * SHAKE_PROBE_ACCEL_WIDTH)                                \
     + (3 * SHAKE_PROBE_LINEAR_WIDTH) + (2 * SHAKE_PROBE_CANDIDATE_WIDTH) + SHAKE_PROBE_DECIMAL_DIGITS(UINT8_MAX))
