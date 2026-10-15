/* mathf_rsqrt: reciprocal square root for positive normal-range inputs; one Newton correction of the FPU seed.
 * CONFIG_MATH_FPU_RECIP enables the Xtensa seed; other builds use exact sqrt. */
#pragma once
#include <math.h>

#ifdef __XTENSA__
#include "sdkconfig.h"
#endif

#define MATHF_RSQRT_RELATIVE_LIMIT 0.0001F

static inline __attribute__((always_inline)) float
mathf_rsqrt(float value) {
#if defined(__XTENSA__) && CONFIG_MATH_FPU_RECIP
    float seed;
    __asm__("rsqrt0.s %0, %1" : "=f"(seed) : "f"(value));
    return seed * (1.5F - 0.5F * value * seed * seed);
#else
    return 1.0F / sqrtf(value);
#endif
}
