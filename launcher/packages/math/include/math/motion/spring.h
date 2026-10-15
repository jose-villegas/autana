/* Shared Q16 spring arithmetic; coefficients and bump weights are out of 256. */
#pragma once

#include <stdint.h>

#define SPRING_COEFFICIENT_ONE 256

static inline int32_t
spring_scale(int32_t value, int coefficient) {
    return (int32_t)(((int64_t)value * coefficient) / SPRING_COEFFICIENT_ONE);
}

/* Squared distance avoids a square root for the radial (1 - t^2)^2 window. */
static inline int
spring_bump_window(int64_t distance_squared, int radius) {
    const int64_t radius_squared = (int64_t)radius * radius;
    if (radius < 1 || distance_squared >= radius_squared) {
        return 0;
    }
    const int t2 = (int)(distance_squared * SPRING_COEFFICIENT_ONE / radius_squared);
    const int remaining = SPRING_COEFFICIENT_ONE - t2;
    return (remaining * remaining) / SPRING_COEFFICIENT_ONE;
}
