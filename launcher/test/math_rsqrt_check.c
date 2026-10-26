/* Exhaustive reciprocal-root bound check. Build with -O3 and the math include
 * directory, link libm, then run; this is separate from the unit-test loop. */
#include <inttypes.h>
#include <math.h>
#include <stdio.h>

#include "math/scalar/mathx.h"

int
main(void) {
    double max_error = 0;
    uint32_t worst = MATHX_RSQRT_MIN;
    const uint64_t end = UINT64_C(1) << MATHX_RSQRT_WORD_BITS;
    const double scale = ldexp(1.0, MATHX_RSQRT_SHIFT + MATHX_RSQRT_WORD_BITS / 2);
    for (uint64_t m = MATHX_RSQRT_MIN; m < end; m++) {
        const double reference = scale / sqrt((double)m);
        const uint32_t expected = mathx_rsqrt_q30_table((uint32_t)m);
        const uint32_t seeds[] = {
            (uint32_t)floor(reference * (1.0 - MATHX_RSQRT_FPU_SEED_RELATIVE_LIMIT)),
            (uint32_t)ceil(reference * (1.0 + MATHX_RSQRT_FPU_SEED_RELATIVE_LIMIT)),
        };
        for (size_t i = 0; i < sizeof seeds / sizeof seeds[0]; i++) {
            const uint32_t got = mathx_rsqrt_bracket((uint32_t)m, mathx_rsqrt_newton((uint32_t)m, seeds[i]));
            if (got != expected) {
                fprintf(stderr, "FAIL m=%" PRIu64 " seed=%" PRIu32 " got=%" PRIu32 " table=%" PRIu32 "\n", m, seeds[i],
                        got, expected);
                return 1;
            }
        }
        const double error = fabs((double)mathx_rsqrt_q30((uint32_t)m) - reference) / reference;
        if (error > max_error) {
            max_error = error;
            worst = (uint32_t)m;
        }
        if (error > MATHX_RSQRT_Q30_RELATIVE_LIMIT) {
            fprintf(stderr, "FAIL m=%" PRIu64 " relative=%.17g\n", m, error);
            return 1;
        }
    }
    printf("PASS max relative error=%.17g at m=%" PRIu32 " limit=%.17g\n", max_error, worst,
           MATHX_RSQRT_Q30_RELATIVE_LIMIT);
    return 0;
}
