/* sh_probes: each grid coefficient is interpolated before cosine convolution. */
#include "render/probe/sh_probes.h"
#include <assert.h>
#include <math.h>
#include <stddef.h>
#include "math/scalar/mathf.h"

bool
sh_probes_inside(const sh_probes_t* probes, int x, int y, int z) {
    assert(x >= 0 && x < probes->dims[0]);
    assert(y >= 0 && y < probes->dims[1]);
    assert(z >= 0 && z < probes->dims[2]);
    const size_t index = ((((size_t)z * probes->dims[1]) + (size_t)y) * probes->dims[0]) + (size_t)x;
    return probes->inside != NULL
           && (probes->inside[index / SH_PROBES_MASK_BITS_PER_BYTE] & (1U << (index % SH_PROBES_MASK_BITS_PER_BYTE)))
                  != 0;
}

sh1_rgb_t
sh_probes_sample(const sh_probes_t* probes, vec3f_t world) {
    const float position[] = {world.x, world.y, world.z};
    const float origin[] = {probes->origin.x, probes->origin.y, probes->origin.z};
    const float spacing[] = {probes->spacing.x, probes->spacing.y, probes->spacing.z};
    int indices[MAT3F_AXES][2];
    float weight[MAT3F_AXES];
    for (int axis = 0; axis < MAT3F_AXES; axis++) {
        assert(probes->dims[axis] > 0);
        assert(probes->dims[axis] == 1 || spacing[axis] > 0);
        const float at = probes->dims[axis] == 1 ? 0.0F
                                                 : mathf_clamp((position[axis] - origin[axis]) / spacing[axis], 0.0F,
                                                               (float)(probes->dims[axis] - 1));
        indices[axis][0] = (int)at;
        indices[axis][1] = indices[axis][0] + (indices[axis][0] < probes->dims[axis] - 1);
        weight[axis] = at - (float)indices[axis][0];
    }
    sh1_rgb_t rows[2][2];
    for (int z = 0; z < 2; z++) {
        for (int y = 0; y < 2; y++) {
            sh1_rgb_t ends[2];
            for (int x = 0; x < 2; x++) {
                const size_t index = ((size_t)indices[2][z] * probes->dims[1] * probes->dims[0])
                                     + ((size_t)indices[1][y] * probes->dims[0]) + (size_t)indices[0][x];
                const int16_t* packed = probes->coefficients[index];
                for (int c = 0; c < SH1_COEFFICIENTS; c++) {
                    ends[x].c[c] = (vec3f_t){(float)packed[c * SH1_CHANNELS] * probes->scale,
                                             (float)packed[(c * SH1_CHANNELS) + 1] * probes->scale,
                                             (float)packed[(c * SH1_CHANNELS) + 2] * probes->scale};
                }
            }
            rows[z][y] = sh1_rgb_lerp(ends[0], ends[1], weight[0]);
        }
    }
    return sh1_rgb_lerp(sh1_rgb_lerp(rows[0][0], rows[0][1], weight[1]),
                        sh1_rgb_lerp(rows[1][0], rows[1][1], weight[1]), weight[2]);
}

_Static_assert(SH_PROBES_HEADER_SIZE % _Alignof(int16_t) == 0, "SHPR coefficient alignment");
