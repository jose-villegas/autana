/* shake_figures_probe: the reader response for the input reference figure. */
#include <stdio.h>

#include "input/motion/shake.h"

#define PROBE_MILLI   1000
#define PROBE_STEPS   100
#define PROBE_STEP_MG ((int)(SHAKE_FULL_G * PROBE_MILLI) / PROBE_STEPS)
#define PROBE_MAX_MG  ((int)(SHAKE_FULL_G * PROBE_MILLI) * 2)

int
main(void) {
    printf("{\"start_mg\":%d,\"full_mg\":%d,\"points\":[", (int)(SHAKE_START_G * PROBE_MILLI),
           (int)(SHAKE_FULL_G * PROBE_MILLI));
    shake_t s;
    shake_reset(&s);
    for (int mg = 0; mg <= PROBE_MAX_MG; mg += PROBE_STEP_MG) {
        s.smooth_g = (float)mg / PROBE_MILLI;
        printf("%s[%d,%g]", mg == 0 ? "" : ",", mg, (double)shake_level(&s));
    }
    puts("]}");
    return 0;
}
