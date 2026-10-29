/* input_fake: host-controlled motion samples and borrowed batches. */
#pragma once

#include "input/input.h"

/* Presence can be set independently after selecting the newest sample. */
void input_fake_motion_present(bool present);

/* Copies a sample and advances its sequence; NULL clears it and marks a missing sensor. */
void input_fake_motion(const input_motion_t* next);

/* Borrows a batch until the caller clears it with NULL. */
void input_fake_motion_batch(const input_motion_batch_t* next);

/* Clears samples, batches and sensor presence; suitable for test cleanup. */
void input_fake_reset(void);

/* Loads a committed gyro offset through the production validation path. */
void input_fake_stored_gyro(vec3i_t offset);
