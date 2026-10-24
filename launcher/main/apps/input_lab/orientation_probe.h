/* orientation_probe: guided script for replaying shell quarter decisions. */
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "input/input.h"
#include "math/scalar/units.h"

#define ORIENTATION_PROBE_STEP_MS     10000U
#define ORIENTATION_PROBE_PROMPT_SIZE 96

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

#define ORIENTATION_PROBE_ROUND_MS (ORIENTATION_PROBE_DONE * ORIENTATION_PROBE_STEP_MS)
#define ORIENTATION_PROBE_RECORD_LINES                                                                                 \
    ((ORIENTATION_PROBE_ROUND_MS * MATH_US_PER_MS + INPUT_MOTION_SAMPLE_US - 1) / INPUT_MOTION_SAMPLE_US               \
     + ORIENTATION_PROBE_DONE + 2)
/* Counts retain offset-calibrated acceleration and raw gyro precision. */
#define ORIENTATION_PROBE_SAMPLE_FORMAT "orientation %d %lld %d %d %d %d %d %d"
int orientation_probe_target(orientation_probe_step_t step);
void orientation_probe_prompt(orientation_probe_step_t step, char* text, size_t size);
