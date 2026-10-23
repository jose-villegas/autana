/* shake_figures_probe: the reader response for the input reference figure. */
#include <stdio.h>

#include "input/motion/shake.h"

#define PROBE_STEP_MG (SHAKE_FULL_MG / 100)
#define PROBE_MAX_MG  (SHAKE_FULL_MG * 2)

int
main(void) {
    printf("{\"start_mg\":%d,\"full_mg\":%d,\"points\":[", SHAKE_START_MG, SHAKE_FULL_MG);
    shake_t s;
    shake_reset(&s);
    for (int mg = 0; mg <= PROBE_MAX_MG; mg += PROBE_STEP_MG) {
        s.smooth_mg = (float)mg;
        printf("%s[%d,%g]", mg == 0 ? "" : ",", mg, (double)shake_level(&s));
    }
    puts("]}");
    return 0;
}
