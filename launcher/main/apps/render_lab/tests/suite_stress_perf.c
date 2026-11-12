/*
 * Stress preset costs along each camera swing, paused in both quarters,
 * with fixed sizes for uniform presets. Stage goals only log distances;
 * each row's r3d sum has a board ceiling. Verdicts are per preset and
 * stage, never summed over presets. Zero means not yet pegged.
 */

#include "suites.h"
#ifdef DEVICE_BUILD
#include <limits.h>
#include <string.h>
#include "apps/render_lab/render_lab.h"
#include "apps/render_lab/render_lab_dynres.h"
#include "apps/render_lab/render_lab_view.h"
#include "apps/render_lab/stress/scene_stress.h"
#include "apps/render_lab/tests/render_lab_test.h"
#include "core/tune.h"
#include "frame/test_frame.h"
#include "lab/test/lab_perf_quarters.h"
#include "render/context/render_context.h"
#include "scene/animator/animator.h"
#include "testkit/cleanup.h"
#include "unity.h"

static const char* TAG = "stress_perf";

static lab_test_t saved;
static const lab_scene_t* preset;
static const lab_perf_quarter_t* quarter;
static int size_step;
static int saved_filter;
static int saved_layout;

typedef struct {
    const char* key;
    const char* quarter;
    const char* size;
    int64_t stage_us[R3D_STAGE_COUNT];
    perf_budget_t r3d;
} stress_goal_t;

/* Worst of a perf-scoped 5-seed capture on A308, 20E8 and 2124, +1%, rounded
 * up to 10 us; ceiling = goal x 1.15, rounded up to 10 us.
 * uniform-full stays unpegged: its full-view uniform fill varies more
 * between runs than any step a change makes. */
static const stress_goal_t stress_goals[] = {
    {"stress-open", "portrait", "default", {0, 0, 0, 0}, {0, 0}},
    {"stress-open", "landscape", "default", {0, 0, 0, 0}, {0, 0}},
    {"stress-stack", "portrait", "default", {0, 0, 0, 0}, {0, 0}},
    {"stress-stack", "landscape", "default", {0, 0, 0, 0}, {0, 0}},
    {"stress-tiny-copies", "portrait", "default", {0, 0, 0, 0}, {0, 0}},
    {"stress-tiny-copies", "landscape", "default", {0, 0, 0, 0}, {0, 0}},
    {"stress-big-tris", "portrait", "default", {0, 0, 0, 0}, {0, 0}},
    {"stress-big-tris", "landscape", "default", {0, 0, 0, 0}, {0, 0}},
    {"stress-per-frame", "portrait", "default", {0, 0, 0, 0}, {0, 0}},
    {"stress-per-frame", "landscape", "default", {0, 0, 0, 0}, {0, 0}},
    {"stress-two-sided", "portrait", "default", {0, 0, 0, 0}, {0, 0}},
    {"stress-two-sided", "landscape", "default", {0, 0, 0, 0}, {0, 0}},
    {"stress-noisy-tiny", "portrait", "default", {0, 0, 0, 0}, {0, 0}},
    {"stress-noisy-tiny", "landscape", "default", {0, 0, 0, 0}, {0, 0}},
    {"stress-mix", "portrait", "default", {0, 0, 0, 0}, {0, 0}},
    {"stress-mix", "landscape", "default", {0, 0, 0, 0}, {0, 0}},
    {"stress-static-copies", "portrait", "default", {0, 0, 0, 0}, {0, 0}},
    {"stress-static-copies", "landscape", "default", {0, 0, 0, 0}, {0, 0}},
    {"stress-uniform-mid", "portrait", "default", {0, 0, 0, 0}, {0, 0}},
    {"stress-uniform-mid", "portrait", "368x448", {0, 0, 0, 0}, {0, 0}},
    {"stress-uniform-mid", "portrait", "368x224", {0, 0, 0, 0}, {0, 0}},
    {"stress-uniform-mid", "landscape", "default", {0, 0, 0, 0}, {0, 0}},
    {"stress-uniform-mid", "landscape", "368x448", {0, 0, 0, 0}, {0, 0}},
    {"stress-uniform-mid", "landscape", "368x224", {0, 0, 0, 0}, {0, 0}},
    {"stress-uniform-full", "portrait", "default", {0, 0, 0, 0}, {0, 0}},
    {"stress-uniform-full", "portrait", "368x448", {0, 0, 0, 0}, {0, 0}},
    {"stress-uniform-full", "portrait", "368x224", {0, 0, 0, 0}, {0, 0}},
    {"stress-uniform-full", "landscape", "default", {0, 0, 0, 0}, {0, 0}},
    {"stress-uniform-full", "landscape", "368x448", {0, 0, 0, 0}, {0, 0}},
    {"stress-uniform-full", "landscape", "368x224", {0, 0, 0, 0}, {0, 0}},
    {"stress-card", "portrait", "default", {0, 0, 0, 0}, {0, 0}},
    {"stress-card", "landscape", "default", {0, 0, 0, 0}, {0, 0}},
    {"stress-culled", "portrait", "default", {0, 0, 0, 0}, {0, 0}},
    {"stress-culled", "landscape", "default", {0, 0, 0, 0}, {0, 0}},
    {"stress-near", "portrait", "default", {0, 0, 0, 0}, {0, 0}},
    {"stress-near", "landscape", "default", {0, 0, 0, 0}, {0, 0}},
    {"stress-few-copies", "portrait", "default", {0, 0, 0, 0}, {0, 0}},
    {"stress-few-copies", "landscape", "default", {0, 0, 0, 0}, {0, 0}},
};

static const stress_goal_t*
stress_goal(void) {
    const char* size = size_step < 0 ? "default" : render_lab_test_step_name(size_step);
    for (size_t i = 0; i < sizeof stress_goals / sizeof stress_goals[0]; i++) {
        const stress_goal_t* goal = &stress_goals[i];
        if (strcmp(goal->key, preset->key) == 0 && strcmp(goal->quarter, quarter->name) == 0
            && strcmp(goal->size, size) == 0) {
            return goal;
        }
    }
    TEST_FAIL_MESSAGE("stress perf row has no goal entry");
    return NULL;
}

static void
report_goals(const stress_goal_t* goal, const int64_t sums[R3D_STAGE_COUNT], int poses) {
    char row[LAB_PERF_ROW_NAME_SIZE];
    int length = snprintf(row, sizeof row, "%s %s%s%s", goal->key, goal->quarter, size_step < 0 ? "" : " ",
                          size_step < 0 ? "" : goal->size);
    TEST_ASSERT_TRUE(length > 0 && (size_t)length < sizeof row);
    int64_t total = 0;
    for (int i = 0; i < R3D_STAGE_COUNT; i++) {
        char stage[LAB_PERF_ROW_NAME_SIZE];
        length = snprintf(stage, sizeof stage, "%s %s", row, r3d_stage_names[i]);
        TEST_ASSERT_TRUE(length > 0 && (size_t)length < sizeof stage);
        const int64_t mean = sums[i] / poses;
        (void)perf_goal(stage, mean, goal->stage_us[i]);
        total += mean;
    }
    lab_perf_named_budget(row, total, total, &goal->r3d, true);
}

static void
release_fixture(void) {
    render_lab_test_show_view(RENDER_DEBUG_VIEW_SHADED);
    render_context_set_dynamic_resolution(render_context_main(), NULL, NULL, 0);
    TEST_ASSERT_TRUE(tune_set(tune_shared(), TUNE_NAME(render_lab, upscale), saved_filter));
    lab_test_exit(&saved);
    TEST_ASSERT_TRUE(tune_set(tune_shared(), TUNE_NAME(render_lab, layout), saved_layout));
}

static void
test_stress_stages(void) {
    const stress_goal_t* goal = stress_goal();
    saved_filter = render_lab_upscale_selected();
    saved_layout = (int)lab_view_layout(&render_lab_view_tunables);
    const stress_params_t* params = preset->data;
    TEST_ASSERT_TRUE(tune_set(tune_shared(), TUNE_NAME(render_lab, layout), LAB_LAYOUT_FULL));
    lab_test_enter(&saved, &render_lab, preset->key);
    suite_set_test_cleanup(release_fixture);
    test_hold_quarter(quarter->quarter);
    (void)lab_test_frame_dt(0, false, NULL);
    if (size_step >= 0) {
        const resolution_step_t size = render_lab_ladder[size_step];
        const resolution_config_t one = resolution_config(&size, 1, 1, INT32_MAX);
        render_context_set_dynamic_resolution(render_context_main(), &one, NULL, 0);
        TEST_ASSERT_TRUE(tune_set(tune_shared(), TUNE_NAME(render_lab, upscale), UPSCALE_FILTER_SURFACE));
    }
    const lab_path_t path = preset->path();
    TEST_ASSERT_FALSE_MESSAGE(handle_is_none(path.scene), "the stress scene did not load");
    const entity_t camera = path.entity;
    TEST_ASSERT_FALSE(handle_is_none(camera));
    TEST_ASSERT_TRUE(animator_seek(camera, ANIMATOR_CURRENT, 0));
    const uint32_t duration_ms = animator_clip_duration_ms(camera, animator_current_clip(camera));
    TEST_ASSERT_GREATER_THAN_UINT32(0, duration_ms);
    int64_t sums[R3D_STAGE_COUNT] = {0};
    for (int i = 0; i < params->perf_poses; i++) {
        int64_t stage_us[R3D_STAGE_COUNT];
        render_lab_test_replay_pose(TAG, camera, duration_ms, (int32_t)((int64_t)duration_ms * i / params->perf_poses),
                                    preset->key, quarter->name, stage_us);
        for (int stage = 0; stage < R3D_STAGE_COUNT; stage++) {
            sums[stage] += stage_us[stage];
        }
    }
    report_goals(goal, sums, params->perf_poses);
    release_fixture();
    suite_clear_test_cleanup();
}
#endif

void
run_stress_perf_suite(void) {
#ifdef DEVICE_BUILD
    for (int i = 0; i < STRESS_PRESET_COUNT; i++) {
        preset = &scene_stress[i];
        const stress_params_t* params = preset->data;
        const int sizes[] = {-1, RENDER_LAB_STEP_FULL, RENDER_LAB_STEP_HALF_HEIGHT};
        const int size_count = params->surface == STRESS_SURFACE_UNIFORM ? (int)(sizeof sizes / sizeof sizes[0]) : 1;
        for (int q = 0; q < (int)LAB_PERF_QUARTER_COUNT; q++) {
            quarter = &lab_perf_quarters[q];
            for (int s = 0; s < size_count; s++) {
                size_step = sizes[s];
                RUN_TEST_CASE(test_stress_stages, preset->key, quarter->name,
                              size_step < 0 ? "default" : render_lab_test_step_name(size_step));
            }
        }
    }
#endif
}

SUITE_REGISTER(run_stress_perf_suite);
