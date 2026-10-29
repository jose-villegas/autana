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

enum { DEPTH_SAMPLES = 64 };

int
main(void) {
    const float code_max = (float)UINT16_MAX;
    const float ratio = powf(DEPTH_LAST / DEPTH_FIRST, 1.0F / (float)(DEPTH_SAMPLES - 1));
    printf("{\"near_z\":%g,\"fits\":[", (double)NEAR_Z);
    for (size_t f = 0; f < sizeof NEAREST / sizeof NEAREST[0]; f++) {
        const float scale = inverse_depth_scale_fit(NEAR_Z, NEAREST[f]);
        printf("%s{\"nearest\":%g,\"scale\":%g,\"depth\":[", f ? "," : "", (double)NEAREST[f], (double)scale);
        float z = DEPTH_FIRST;
        for (int i = 0; i < DEPTH_SAMPLES; i++, z *= ratio) {
            printf("%s%g", i ? "," : "", (double)z);
        }
        printf("],\"step\":[");
        z = DEPTH_FIRST;
        for (int i = 0; i < DEPTH_SAMPLES; i++, z *= ratio) {
            printf("%s%g", i ? "," : "", 1.0 / (double)inverse_depth_steps_per_unit(scale, code_max, z));
        }
        printf("]}");
    }
    printf("]}\n");
    return 0;
}
