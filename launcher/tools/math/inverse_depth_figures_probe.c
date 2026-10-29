/* inverse_depth_figures_probe: one 16-bit inverse-depth step against view
 * depth, at the clip plane's scale and at the scale inverse_depth_scale_fit()
 * gives for a few nearest depths. Lengths in centimetres. */
#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "math/projection/inverse_depth.h"

static const float NEAR_Z = 6.0F;
/* The nearest thing in the picture: at the clip plane, a street walk, a view from above. */
static const float NEAREST[] = {6.0F, 240.0F, 650.0F};
static const float DEPTH_FIRST = 100.0F;
static const float DEPTH_LAST = 6000.0F;
/* The ground's distance from a view 26 m up, where the figure reads each step. */
static const float GUIDE_DEPTH = 3000.0F;

enum { DEPTH_SAMPLES = 64 };

int
main(void) {
    const float code_max = (float)UINT16_MAX;
    printf("{\"near_z\":%g,\"guide\":%g,\"fits\":[", (double)NEAR_Z, (double)GUIDE_DEPTH);
    for (size_t f = 0; f < sizeof NEAREST / sizeof NEAREST[0]; f++) {
        const float scale = inverse_depth_scale_fit(NEAR_Z, NEAREST[f]);
        /* A code stands for depths at or past its scale only. */
        const float first = fmaxf(DEPTH_FIRST, scale);
        const float ratio = powf(DEPTH_LAST / first, 1.0F / (float)(DEPTH_SAMPLES - 1));
        printf("%s{\"nearest\":%g,\"scale\":%g,\"at_guide\":%g,\"depth\":[", f ? "," : "", (double)NEAREST[f],
               (double)scale, 1.0 / (double)inverse_depth_steps_per_unit(scale, code_max, GUIDE_DEPTH));
        float z = first;
        for (int i = 0; i < DEPTH_SAMPLES; i++, z *= ratio) {
            printf("%s%g", i ? "," : "", (double)z);
        }
        printf("],\"step\":[");
        z = first;
        for (int i = 0; i < DEPTH_SAMPLES; i++, z *= ratio) {
            printf("%s%g", i ? "," : "", 1.0 / (double)inverse_depth_steps_per_unit(scale, code_max, z));
        }
        printf("]}");
    }
    printf("]}\n");
    return 0;
}
