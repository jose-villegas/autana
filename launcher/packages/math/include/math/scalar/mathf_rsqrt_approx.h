/* Positive normal-range inputs only; one Newton correction of the FPU seed.
 * Device sweeps gate relative error and lighting-byte error; host/QEMU use exact sqrt. */
#pragma once
#include <math.h>

#define MATHF_RSQRT_APPROX_RELATIVE_LIMIT 0.0001F

static inline __attribute__((always_inline)) float
mathf_rsqrt_approx(float value) {
#if defined(__XTENSA__) && !CONFIG_LAUNCHER_QEMU
    float seed;
    __asm__("rsqrt0.s %0, %1" : "=f"(seed) : "f"(value));
    return seed * (1.5F - 0.5F * value * seed * seed);
#else
    return 1.0F / sqrtf(value);
#endif
}
