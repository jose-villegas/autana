/* Timed render windows cover both upright panel orientations. */
#pragma once

#include <stdio.h>
#include "display/display.h"
#include "frame_profile.h"
#include "perf_budget.h"

typedef struct {
    const char* name;
    int quarter;
} lab_perf_quarter_t;

static const lab_perf_quarter_t lab_perf_quarters[] = {
    {DISPLAY_PORTRAIT_NAME, DISPLAY_PORTRAIT},
    {DISPLAY_LANDSCAPE_NAME, DISPLAY_LANDSCAPE},
};
#define LAB_PERF_QUARTER_FORMAT " " FRAME_PROFILE_QUARTER "%s"
#define LAB_PERF_QUARTER_COUNT  (sizeof lab_perf_quarters / sizeof lab_perf_quarters[0])

#define LAB_PERF_ROW_NAME_SIZE  96

/* Logs the mean's distance to its goal; with `enforce_ceiling`, fails when
 * the p95 is over the ceiling. */
static inline __attribute__((always_inline)) void
lab_perf_named_budget(const char* name, int64_t mean_us, int64_t p95_us, const perf_budget_t* budget,
                      bool enforce_ceiling) {
    (void)perf_goal(name, mean_us, budget->mean_goal_us);
    if (enforce_ceiling) {
        perf_guard(name, p95_us, budget->p95_ceiling_us);
    }
}

static inline __attribute__((always_inline)) void
lab_perf_budget(const char* row, const lab_perf_quarter_t* quarter, int64_t mean_us, int64_t p95_us,
                const perf_budget_t* budget, bool enforce_ceiling) {
    char name[LAB_PERF_ROW_NAME_SIZE];
    const int length = snprintf(name, sizeof name, "%s %s", row, quarter->name);
    TEST_ASSERT_TRUE(length > 0 && (size_t)length < sizeof name);
    lab_perf_named_budget(name, mean_us, p95_us, budget, enforce_ceiling);
}
