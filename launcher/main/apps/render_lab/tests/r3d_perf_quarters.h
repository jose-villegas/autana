/* Timed render windows cover both upright panel orientations. */
#pragma once

#include "display/display_shell.h"

typedef struct {
    const char* name;
    int quarter;
} r3d_perf_quarter_t;

static const r3d_perf_quarter_t r3d_perf_quarters[] = {
    {"portrait", DISPLAY_PORTRAIT},
    {"landscape", DISPLAY_LANDSCAPE},
};
#define R3D_PERF_QUARTER_COUNT (sizeof r3d_perf_quarters / sizeof r3d_perf_quarters[0])
