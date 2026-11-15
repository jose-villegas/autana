/* sh_probes: each grid coefficient is interpolated before cosine convolution. */
#include "render/probe/sh_probes.h"
#include <assert.h>
#include <math.h>
#include <stddef.h>
#include "math/scalar/mathf.h"

sh1_rgb_t
sh_probes_sample(const sh_probes_t* probes, vec3f_t world) {
    const float position[] = {world.x, world.y, world.z};
    const float origin[] = {probes->origin.x, probes->origin.y, probes->origin.z};
    const float spacing[] = {probes->spacing.x, probes->spacing.y, probes->spacing.z};
    int lo[MAT3F_AXES], hi[MAT3F_AXES];
    float weight[MAT3F_AXES];
    for (int axis = 0; axis < MAT3F_AXES; axis++) {
        assert(probes->dims[axis] > 0);
        assert(probes->dims[axis] == 1 || spacing[axis] > 0);
        const float at = probes->dims[axis] == 1 ? 0.0F
                                                 : mathf_clamp((position[axis] - origin[axis]) / spacing[axis], 0.0F,
                                                               (float)(probes->dims[axis] - 1));
        lo[axis] = (int)at;
        hi[axis] = lo[axis] + (lo[axis] < probes->dims[axis] - 1);
        weight[axis] = at - (float)lo[axis];
    }
    sh1_rgb_t rows[2][2];
    for (int z = 0; z < 2; z++) {
        for (int y = 0; y < 2; y++) {
            sh1_rgb_t ends[2];
            for (int x = 0; x < 2; x++) {
                const size_t index = (size_t)(z ? hi[2] : lo[2]) * probes->dims[1] * probes->dims[0]
                                     + (size_t)(y ? hi[1] : lo[1]) * probes->dims[0] + (size_t)(x ? hi[0] : lo[0]);
                const int16_t* packed = probes->coefficients[index];
                for (int c = 0; c < SH1_COEFFICIENTS; c++) {
                    ends[x].c[c] = (vec3f_t){packed[c * SH1_CHANNELS] * probes->scale,
                                             packed[c * SH1_CHANNELS + 1] * probes->scale,
                                             packed[c * SH1_CHANNELS + 2] * probes->scale};
                }
            }
            rows[z][y] = sh1_rgb_lerp(ends[0], ends[1], weight[0]);
        }
    }
    return sh1_rgb_lerp(sh1_rgb_lerp(rows[0][0], rows[0][1], weight[1]),
                        sh1_rgb_lerp(rows[1][0], rows[1][1], weight[1]), weight[2]);
}
