/* The upright panel orientations every timed render window covers, apart
 * from the test kit, so a host tool runs what a board's perf suite runs. */
#pragma once

#include "display/display.h"

typedef struct {
    const char* name;
    int quarter;
} lab_perf_quarter_t;

static const lab_perf_quarter_t lab_perf_quarters[] = {
    {DISPLAY_PORTRAIT_NAME, DISPLAY_PORTRAIT},
    {DISPLAY_LANDSCAPE_NAME, DISPLAY_LANDSCAPE},
};
#define LAB_PERF_QUARTER_COUNT (sizeof lab_perf_quarters / sizeof lab_perf_quarters[0])
