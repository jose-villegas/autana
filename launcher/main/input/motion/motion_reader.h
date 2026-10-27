/* motion_reader: sample cache for the shared gravity and shake readers. */
#pragma once
#include <stdint.h>
#include "input/input.h"

/* Copies calibrated motion and its sequence from the same newest sample. */
bool input_read_motion_sequence(input_motion_t* out, uint32_t* sequence);
/* Driver start and isolated host fixtures only; app switches retain state. */
void motion_reader_reset(void);

/* True when a rest residual was applied; false when rejected without changing the offset. */
bool input_learn_gyro_offset(vec3i_t residual);
