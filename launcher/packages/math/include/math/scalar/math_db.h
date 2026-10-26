/* Floored decibel conversions for nonnegative ratios. */
#pragma once
#include <math.h>

#define MATH_DB_FLOOR           (-120.0F)
#define MATH_DB_POWER_SCALE     10.0F
#define MATH_DB_AMPLITUDE_SCALE 20.0F

static inline float
math_db_power(float ratio) {
    return fmaxf(MATH_DB_FLOOR, MATH_DB_POWER_SCALE * log10f(ratio));
}

static inline float
math_db_amplitude(float ratio) {
    return fmaxf(MATH_DB_FLOOR, MATH_DB_AMPLITUDE_SCALE * log10f(ratio));
}
