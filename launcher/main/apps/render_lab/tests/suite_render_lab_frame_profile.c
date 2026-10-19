#include "suites.h"
#ifdef DEVICE_BUILD
#include <string.h>
#include "apps/render_lab/sponza_content.h"
#include "apps/render_lab/tests/r3d_perf_quarters.h"
#include "apps/render_lab/tests/render_lab_test.h"
#include "core/timing.h"
#include "frame_profile.h"
#include "perf_budget.h"
#include "scene/scene_internal.h"
#include "test_cleanup.h"
#include "unity.h"

typedef struct {
    const char* row;
    const char* pack;
    const char* entity;
    const char* command;
    perf_budget_t quarters[R3D_PERF_QUARTER_COUNT];
} profile_scenario_t;

static const profile_scenario_t profile_scenarios[] = {
    {.row = "sponza"},
    {.row = "sponza-lite"},
    {.row = "sponza-flat"},
    {.row = "sponza-fitted"},
    {.row = "sponza-fitted-full"},
    {.row = "sponza-flat-fitted"},
    {"capybara", "capybara", "capybara", "clip gallop 0", {{0}}},
};

static render_lab_test_t saved;

static void
release_fixture(void) {
    render_lab_test_exit(&saved);
}

static int64_t
whole_frame(void* ctx, int64_t dt_us) {
    int64_t* remainder_us = ctx;
    const int64_t step_us = dt_us + *remainder_us;
    const uint32_t dt_ms = (uint32_t)(step_us / FRAME_PROFILE_US_PER_MS);
    *remainder_us = step_us % FRAME_PROFILE_US_PER_MS;
    const int64_t start = timing_now_us();
    (void)render_lab_test_frame_dt(dt_ms, false, NULL);
    return timing_now_us() - start;
}

static void
test_real_time_frame_profiles(void) {
    for (size_t i = 0; i < sizeof profile_scenarios / sizeof profile_scenarios[0]; i++) {
        const profile_scenario_t* scenario = &profile_scenarios[i];
        for (size_t q = 0; q < R3D_PERF_QUARTER_COUNT; q++) {
            render_lab_test_enter(&saved, scenario->row);
            suite_set_test_cleanup(release_fixture);
            render_lab_test_hold_quarter(r3d_perf_quarters[q].quarter);
            scene_t* scene = render_lab_test_loaded_scene(scenario->pack == NULL ? SPONZA_SCENE : scenario->pack);
            TEST_ASSERT_NOT_NULL(scene);
            const scene_entity_t entity =
                scene_find(scene, scenario->entity == NULL ? sponza_cameras[0] : scenario->entity);
            TEST_ASSERT_NOT_EQUAL(SCENE_ENTITY_NONE, entity);
            if (scenario->command != NULL) {
                TEST_ASSERT_TRUE(app_render_lab.console->handle(scenario->command));
            }
            TEST_ASSERT_TRUE(scene_animator_seek(scene, entity, SCENE_ANIMATOR_CURRENT, 0));
            const int clip = scene->animators[entity].layer.current.clip;
            const uint32_t duration_ms = scene_animator_clip_duration_ms(scene, entity, clip);
            TEST_ASSERT_GREATER_THAN_UINT32(0, duration_ms);
            const frame_profile_scenario_t playback = {
                scenario->row,
                (int64_t)duration_ms * FRAME_PROFILE_US_PER_MS,
            };
            int64_t remainder_us = 0;
            const frame_profile_t p = frame_profile_play(&playback, whole_frame, &remainder_us);
            frame_profile_log("render_lab_profile", scenario->row, &p, r3d_perf_quarters[q].name);
            TEST_ASSERT_GREATER_THAN_INT_MESSAGE(0, p.frames, "frame profile failed");
            (void)perf_goal(scenario->row, p.frame_us.avg, scenario->quarters[q].mean_goal_us);
            perf_guard(scenario->row, p.frame_us.p95, scenario->quarters[q].p95_ceiling_us);
            release_fixture();
            suite_clear_test_cleanup();
        }
    }
}
#endif

void
run_render_lab_frame_profile_suite(void) {
#ifdef DEVICE_BUILD
    RUN_TEST(test_real_time_frame_profiles);
#endif
}

SUITE_REGISTER(run_render_lab_frame_profile_suite);
