/* shake_figures_probe: the reader response for the input reference figures.
 * With a samples file (one "step ms ax ay az lx ly lz" line in mg per motion
 * sample), it also runs those linear accelerations through the reader. */
#include <math.h>
#include <stdio.h>

#include "input/input.h"
#include "input/motion/shake.h"

#define PROBE_MILLI   1000
#define PROBE_STEPS   100
#define PROBE_STEP_MG ((int)(SHAKE_FULL_G * PROBE_MILLI) / PROBE_STEPS)
#define PROBE_MAX_MG  ((int)(SHAKE_FULL_G * PROBE_MILLI) * 2)
#define PROBE_NAME    16

static int
trace(const char* path) {
    FILE* in = fopen(path, "r");
    if (in == NULL) {
        perror(path);
        return 1;
    }
    shake_t s;
    shake_reset(&s);
    char step[PROBE_NAME];
    unsigned ms;
    int lx, ly, lz;
    const char* separator = "";
    printf(",\"trace\":[");
    while (fscanf(in, "%15s %u %*d %*d %*d %d %d %d", step, &ms, &lx, &ly, &lz) == 5) {
        const vec3f_t linear = {(float)lx / PROBE_MILLI, (float)ly / PROBE_MILLI, (float)lz / PROBE_MILLI};
        shake_update(&s, linear, INPUT_MOTION_SAMPLE_US);
        printf("%s[\"%s\",%u,%g,%g,%g]", separator, step, ms, (double)sqrtf(vec3f_dot(linear, linear)) * PROBE_MILLI,
               (double)s.smooth_g * PROBE_MILLI, (double)shake_level(&s));
        separator = ",";
    }
    printf("]");
    fclose(in);
    return 0;
}

int
main(int argc, char** argv) {
    printf("{\"tau_ms\":%u,\"sample_us\":%u,\"start_mg\":%d,\"full_mg\":%d,\"points\":[", SHAKE_TAU_MS,
           INPUT_MOTION_SAMPLE_US, (int)(SHAKE_START_G * PROBE_MILLI), (int)(SHAKE_FULL_G * PROBE_MILLI));
    shake_t s;
    shake_reset(&s);
    for (int mg = 0; mg <= PROBE_MAX_MG; mg += PROBE_STEP_MG) {
        s.smooth_g = (float)mg / PROBE_MILLI;
        printf("%s[%d,%g]", mg == 0 ? "" : ",", mg, (double)shake_level(&s));
    }
    printf("]");
    if (argc > 1 && trace(argv[1]) != 0) {
        return 1;
    }
    puts("}");
    return 0;
}
