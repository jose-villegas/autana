/* input_fake: host-controlled motion batches; NULL marks frames without a batch. */
#pragma once

#include "input/input.h"

/* Newest sample; NULL marks a missing sensor. The caller retains ownership. */
void input_fake_motion(const input_motion_t* next);

void input_fake_motion_batch(const input_motion_batch_t* next);
