/* input_fake: host-controlled motion batches; NULL marks frames without a batch. */
#pragma once

#include "input/input.h"

void input_fake_motion_batch(const input_motion_batch_t* next);
