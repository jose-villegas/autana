/* Timed render windows cover both upright panel orientations. */
#pragma once

#include <stdio.h>
#include "display/display_shell.h"
#include "perf_budget.h"

typedef struct {
    const char* name;
    int quarter;
} r3d_perf_quarter_t;

static const r3d_perf_quarter_t r3d_perf_quarters[] = {
    {DISPLAY_PORTRAIT_NAME, DISPLAY_PORTRAIT},
    {DISPLAY_LANDSCAPE_NAME, DISPLAY_LANDSCAPE},
};
#define R3D_PERF_QUARTER_FORMAT " quarter=%s"
#define R3D_PERF_QUARTER_COUNT  (sizeof r3d_perf_quarters / sizeof r3d_perf_quarters[0])

#define R3D_PERF_ROW_NAME_SIZE  96

static inline __attribute__((always_inline)) void
r3d_perf_budget(const char* row, const r3d_perf_quarter_t* quarter, int64_t mean_us, int64_t p95_us,
                const perf_budget_t* budget) {
    char name[R3D_PERF_ROW_NAME_SIZE];
    const int length = snprintf(name, sizeof name, "%s %s", row, quarter->name);
    TEST_ASSERT_TRUE(length > 0 && (size_t)length < sizeof name);
    (void)perf_goal(name, mean_us, budget->mean_goal_us);
    perf_guard(name, p95_us, budget->p95_ceiling_us);
}
