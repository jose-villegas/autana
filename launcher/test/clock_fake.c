#include "clock_fake.h"
#include "math/scalar/units.h"
static bool enabled;
static int64_t clock_us;
static const int64_t* readings;
static size_t remaining;

void
clock_fake_set(int64_t now_us) {
    enabled = true;
    clock_us = now_us;
    remaining = 0;
}

void
clock_fake_script(const int64_t* readings_us, size_t count) {
    enabled = true;
    readings = readings_us;
    remaining = count;
}

void
clock_fake_reset(void) {
    enabled = false;
    remaining = 0;
}

bool
clock_fake_read(struct timespec* now) {
    if (!enabled) {
        return false;
    }
    if (remaining != 0) {
        clock_us = *readings++;
        remaining--;
    }
    now->tv_sec = clock_us / MATH_US_PER_S;
    now->tv_nsec = (long)(clock_us % MATH_US_PER_S) * MATH_NS_PER_US;
    return true;
}
