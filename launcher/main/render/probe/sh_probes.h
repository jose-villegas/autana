/* sh_probes: clamped trilinear sampling of packed RGB L1 radiance grids. */
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "math/sh/sh1.h"
#define SH_PROBES_COMPONENTS         (SH1_COEFFICIENTS * SH1_CHANNELS)

#define SH_PROBES_NAME_SUFFIX        ".shpr"

#define SH_PROBES_VERSION            1U
#define SH_PROBES_FLAG_INSIDE_MASK   0x0001U
#define SH_PROBES_MASK_BITS_PER_BYTE 8U

enum {
    SH_PROBES_VERSION_OFFSET = 0,
    SH_PROBES_FLAGS_OFFSET = SH_PROBES_VERSION_OFFSET + sizeof(uint16_t),
    SH_PROBES_ORIGIN_OFFSET = SH_PROBES_FLAGS_OFFSET + sizeof(uint16_t),
    SH_PROBES_SPACING_OFFSET = SH_PROBES_ORIGIN_OFFSET + MAT3F_AXES * sizeof(float),
    SH_PROBES_SCALE_OFFSET = SH_PROBES_SPACING_OFFSET + MAT3F_AXES * sizeof(float),
    SH_PROBES_DIMS_OFFSET = SH_PROBES_SCALE_OFFSET + sizeof(float),
    SH_PROBES_PADDING_OFFSET = SH_PROBES_DIMS_OFFSET + MAT3F_AXES * sizeof(uint16_t),
    SH_PROBES_HEADER_SIZE = SH_PROBES_PADDING_OFFSET + sizeof(uint16_t),
    SH_PROBES_RECORD_SIZE = SH_PROBES_COMPONENTS * sizeof(int16_t)
};

typedef struct sh_probes {
    vec3f_t origin, spacing;
    uint16_t dims[MAT3F_AXES]; /* nonzero, x fastest; a singleton axis ignores its spacing */
    float scale;
    const uint8_t* inside;
    const int16_t (*coefficients)[SH_PROBES_COMPONENTS];
} sh_probes_t;

bool sh_probes_inside(const sh_probes_t* probes, int x, int y, int z);

sh1_rgb_t sh_probes_sample(const sh_probes_t* probes, vec3f_t world);
