/* sh_probes: clamped trilinear sampling of packed RGB L1 radiance grids. */
#pragma once
#include <stdint.h>
#include "math/sh/sh1.h"
#define SH_PROBES_COMPONENTS (SH1_COEFFICIENTS * SH1_CHANNELS)

typedef struct {
    vec3f_t origin, spacing;
    uint16_t dims[3]; /* nonzero, x fastest; a singleton axis ignores its spacing */
    float scale;
    const int16_t (*coefficients)[SH_PROBES_COMPONENTS];
} sh_probes_t;

sh1_rgb_t sh_probes_sample(const sh_probes_t* probes, vec3f_t world);
