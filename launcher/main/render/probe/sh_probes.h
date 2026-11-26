/* sh_probes: clamped trilinear sampling of packed RGB L1 radiance grids. */
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "asset/asset_entry.h"
#include "math/sh/sh1.h"
#define SH_PROBES_COMPONENTS         (SH1_COEFFICIENTS * SH1_CHANNELS)

#define SH_PROBES_NAME_SUFFIX        ".shpr"

#define SH_PROBES_ASSET              ASSET_TYPE('S', 'H', 'P', 'R')
#define SH_PROBES_VERSION            1U
#define SH_PROBES_FLAG_INSIDE_MASK   0x0001U
#define SH_PROBES_MASK_BITS_PER_BYTE 8U

#define SH_PROBES_AT_VERSION         0U
#define SH_PROBES_AT_FLAGS           2U
#define SH_PROBES_AT_ORIGIN          4U
#define SH_PROBES_AT_SPACING         16U
#define SH_PROBES_AT_SCALE           28U
#define SH_PROBES_AT_DIMS            32U
#define SH_PROBES_AT_PADDING         38U
#define SH_PROBES_HEADER_SIZE        40U

enum { SH_PROBES_RECORD_SIZE = SH_PROBES_COMPONENTS * sizeof(int16_t) };

typedef struct sh_probes {
    vec3f_t origin, spacing;
    uint16_t dims[MAT3F_AXES]; /* nonzero, x fastest; a singleton axis ignores its spacing */
    float scale;
    const uint8_t* inside;
    const int16_t (*coefficients)[SH_PROBES_COMPONENTS];
} sh_probes_t;

bool sh_probes_inside(const sh_probes_t* probes, int x, int y, int z);

sh1_rgb_t sh_probes_sample(const sh_probes_t* probes, vec3f_t world);

/* The entry bytes must outlive the grid. Failure clears the output. */
asset_status_t sh_probes_read(asset_view_t entry, sh_probes_t* out);
