/* A mean goal only logs its distance from the measurement. A p95 ceiling fails the test on the board
 * once the test ends, so every later row is still measured; it only logs under QEMU. Zero means not
 * yet pegged. */
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "esp_log.h"
#include "testkit/cleanup.h"
#include "unity.h"

#define PERF_BUDGET_TAG     "device_tests"
#define PERF_BUDGET_PERCENT 100

typedef struct {
    int64_t mean_goal_us;
    int64_t p95_ceiling_us;
} perf_budget_t;

static inline bool
perf_guard_passes(int64_t measured_us, int64_t ceiling_us) {
#if CONFIG_LAUNCHER_QEMU
    (void)measured_us;
    (void)ceiling_us;
    return true;
#else
    return ceiling_us == 0 || measured_us < ceiling_us;
#endif
}

static inline void
perf_guard(const char* name, int64_t measured_us, int64_t ceiling_us) {
    if (ceiling_us == 0) {
        ESP_LOGI(PERF_BUDGET_TAG, "%s: ceiling not yet pegged (measured_us=%lld)", name, (long long)measured_us);
        return;
    }
#if CONFIG_LAUNCHER_QEMU
    ESP_LOGI(PERF_BUDGET_TAG, "%s: %lld us, board ceiling %lld us not enforced", name, (long long)measured_us,
             (long long)ceiling_us);
#else
    if (!perf_guard_passes(measured_us, ceiling_us)) {
        ESP_LOGE(PERF_BUDGET_TAG, "%s: %lld us, over its ceiling %lld us", name, (long long)measured_us,
                 (long long)ceiling_us);
        suite_fail_after_test(name);
    }
#endif
}

static inline bool
perf_goal(const char* name, int64_t measured_us, int64_t goal_us) {
    if (goal_us == 0) {
        ESP_LOGI(PERF_BUDGET_TAG, "PERF TARGET %s: measured %lld us, goal not yet pegged", name,
                 (long long)measured_us);
        return false;
    }
    const int64_t distance_percent = ((measured_us - goal_us) * PERF_BUDGET_PERCENT) / goal_us;
    ESP_LOGI(PERF_BUDGET_TAG, "PERF TARGET %s: measured %lld us, goal %lld us, distance %+lld%%", name,
             (long long)measured_us, (long long)goal_us, (long long)distance_percent);
    return measured_us > goal_us;
}
