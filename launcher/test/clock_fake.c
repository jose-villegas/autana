#include "clock_fake.h"
#include "math/scalar/units.h"
#define CLOCK_NS_PER_US 1000
static bool enabled;
static int64_t clock_us;

void
clock_fake_set(int64_t now_us) {
    enabled = true;
    clock_us = now_us;
}

void
clock_fake_reset(void) {
    enabled = false;
}

bool
clock_fake_read(struct timespec* now) {
    if (!enabled) {
        return false;
    }
    now->tv_sec = clock_us / MATH_US_PER_S;
    now->tv_nsec = (long)(clock_us % MATH_US_PER_S) * CLOCK_NS_PER_US;
    return true;
}
