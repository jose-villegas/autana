#include "input_fake.h"

#include <stddef.h>
#include "input/motion/imu.h"
#include "input/motion/motion_reader.h"

static const input_motion_batch_t* motion_batch;
static const input_motion_t* motion;
static bool sensor_present;
static uint32_t motion_sequence;

void
input_fake_motion_present(bool present) {
    sensor_present = present;
}

bool
imu_ready(void) {
    return sensor_present;
}

void
input_fake_motion(const input_motion_t* next) {
    motion = next;
    motion_sequence++;
    sensor_present = next != NULL;
}

bool
input_read_motion(input_motion_t* out) {
    if (motion == NULL) {
        return false;
    }
    *out = *motion;
    return true;
}

bool
input_read_motion_sequence(input_motion_t* out, uint32_t* sequence) {
    if (!input_read_motion(out)) {
        return false;
    }
    *sequence = motion_sequence;
    return true;
}

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
