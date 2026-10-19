/* Board goals report distance; ceilings are not enforced in an emulator. */
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "esp_log.h"
#include "unity.h"

#define PERF_BUDGET_PERCENT 100

typedef struct {
    int64_t mean_goal_us;
    int64_t p95_ceiling_us;
} perf_budget_t;

static inline void
perf_guard(const char* name, int64_t measured_us, int64_t ceiling_us) {
    if (ceiling_us == 0) {
        ESP_LOGI("device_tests", "%s: %lld us, ceiling not yet pegged", name, (long long)measured_us);
        return;
    }
#if CONFIG_LAUNCHER_QEMU
    ESP_LOGI("device_tests", "%s: %lld us, board ceiling %lld us not enforced", name, (long long)measured_us,
             (long long)ceiling_us);
#else
    TEST_ASSERT_LESS_THAN_INT64_MESSAGE(ceiling_us, measured_us, name);
#endif
}

static inline bool
perf_goal(const char* name, int64_t measured_us, int64_t goal_us) {
    if (goal_us == 0) {
        ESP_LOGI("device_tests", "PERF TARGET %s: measured %lld us, goal not yet pegged", name, (long long)measured_us);
        return false;
    }
    const int64_t distance_percent = ((measured_us - goal_us) * PERF_BUDGET_PERCENT) / goal_us;
    ESP_LOGI("device_tests", "PERF TARGET %s: measured %lld us, goal %lld us, distance %+lld%%", name,
             (long long)measured_us, (long long)goal_us, (long long)distance_percent);
    return measured_us > goal_us;
}
