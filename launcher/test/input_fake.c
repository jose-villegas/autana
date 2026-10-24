#include "input_fake.h"

#include <stddef.h>

static const input_motion_batch_t* motion_batch;

void
input_fake_motion_batch(const input_motion_batch_t* next) {
    motion_batch = next;
}

bool
input_read_motion_batch(input_motion_batch_t* out) {
    if (motion_batch == NULL) {
        return false;
    }
    *out = *motion_batch;
    return true;
}
