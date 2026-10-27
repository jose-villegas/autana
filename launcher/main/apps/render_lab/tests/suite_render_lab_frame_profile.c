#include "suites.h"
#ifdef DEVICE_BUILD
#include <stdarg.h>
#include <stdio.h>
#include "apps/render_lab/capybara_content.h"
#include "apps/render_lab/render_lab_scene.h"
#include "apps/render_lab/scene_baked.h"
#include "apps/render_lab/sponza_content.h"
#include "apps/render_lab/tests/r3d_perf_quarters.h"
#include "apps/render_lab/tests/render_lab_test.h"
#include "core/timing.h"
#include "frame_profile.h"
#include "perf_budget.h"
#include "scene/scene_internal.h"
#include "testkit/cleanup.h"
#include "unity.h"

/* A capybara row: the view it is seen from and how the gallop comes in. A
 * cut times one gallop cycle; a blend times only the blend, so every timed
 * frame samples and mixes two clips. */
typedef struct {
    const char* row;
    const char* view;
    int blend_ms;
} capybara_take_t;

typedef struct {
    const render_lab_scene_t* scene;
    const char* pack;
    const char* entity;
    const capybara_take_t* take;
    perf_budget_t quarters[R3D_PERF_QUARTER_COUNT];
} profile_scenario_t;

static const capybara_take_t capybara_home = {CAPYBARA_SCENE, CAPYBARA_VIEW_HOME, CAPYBARA_CLIP_CUT_MS};
static const capybara_take_t capybara_front = {"capybara-front", CAPYBARA_VIEW_FRONT, CAPYBARA_CLIP_CUT_MS};
static const capybara_take_t capybara_side = {"capybara-side", CAPYBARA_VIEW_SIDE, CAPYBARA_CLIP_CUT_MS};
static const capybara_take_t capybara_back = {"capybara-back", CAPYBARA_VIEW_BACK, CAPYBARA_CLIP_CUT_MS};
static const capybara_take_t capybara_close = {"capybara-close", CAPYBARA_VIEW_CLOSE, CAPYBARA_CLIP_CUT_MS};
static const capybara_take_t capybara_blend = {"capybara-blend", CAPYBARA_VIEW_HOME, CAPYBARA_CLIP_BLEND_MS};

/* Worst of a 5-seed capture on A308 (before and after its move), 20E8 and
 * 2124, + 1%; per quarter, portrait then landscape. */
static const profile_scenario_t profile_scenarios[] = {
    {.scene = &scene_sponza[SCENE_BAKED_FULL], .quarters = {{48953, 59273}, {47229, 56329}}},
    {.scene = &scene_sponza[SCENE_BAKED_LITE], .quarters = {{41957, 47931}, {40738, 46118}}},
    {.scene = &scene_sponza[SCENE_BAKED_FLAT], .quarters = {{40902, 49780}, {40227, 48295}}},
    {.scene = &scene_sponza[SCENE_BAKED_FITTED], .quarters = {{41341, 49565}, {39726, 46664}}},
    {.scene = &scene_sponza[SCENE_BAKED_FITTED_FULL], .quarters = {{49536, 62165}, {47469, 58108}}},
    {.scene = &scene_sponza[SCENE_BAKED_FLAT_FITTED], .quarters = {{40285, 50300}, {39053, 48179}}},
    {&scene_capybara, CAPYBARA_SCENE, CAPYBARA_SUBJECT, &capybara_home, {{20941, 21286}, {21500, 21803}}},
    {&scene_capybara, CAPYBARA_SCENE, CAPYBARA_SUBJECT, &capybara_front, {{0, 0}, {0, 0}}},
    {&scene_capybara, CAPYBARA_SCENE, CAPYBARA_SUBJECT, &capybara_side, {{0, 0}, {0, 0}}},
    {&scene_capybara, CAPYBARA_SCENE, CAPYBARA_SUBJECT, &capybara_back, {{0, 0}, {0, 0}}},
    {&scene_capybara, CAPYBARA_SCENE, CAPYBARA_SUBJECT, &capybara_close, {{0, 0}, {0, 0}}},
    {&scene_capybara, CAPYBARA_SCENE, CAPYBARA_SUBJECT, &capybara_blend, {{0, 0}, {0, 0}}},
};

static render_lab_test_t saved;

static void
release_fixture(void) {
    render_lab_test_exit(&saved);
}

/* One console line, formatted, for the showing scene. */
static void
send(const char* format, ...) {
    char command[ASSET_NAME_MAX];
    va_list args;
    va_start(args, format);
    const int length = vsnprintf(command, sizeof command, format, args);
    va_end(args);
    TEST_ASSERT_TRUE(length > 0 && (size_t)length < sizeof command);
    TEST_ASSERT_TRUE(app_render_lab.console->handle(command));
}

static int64_t
whole_frame(void* ctx, int64_t dt_us) {
    int64_t* remainder_us = ctx;
    const int64_t step_us = dt_us + *remainder_us;
    const uint32_t dt_ms = (uint32_t)(step_us / MATH_US_PER_MS);
    *remainder_us = step_us % MATH_US_PER_MS;
    const int64_t start = timing_now_us();
    (void)render_lab_test_frame_dt(dt_ms, false, NULL);
    return timing_now_us() - start;
}

/* Every row is measured before the first one over its p95 ceiling fails the test. */
static void
test_real_time_frame_profiles(void) {
    char over[R3D_PERF_ROW_NAME_SIZE] = "";
    int64_t over_p95_us = 0;
    int64_t over_ceiling_us = 0;
    for (size_t i = 0; i < sizeof profile_scenarios / sizeof profile_scenarios[0]; i++) {
        const profile_scenario_t* scenario = &profile_scenarios[i];
        for (size_t q = 0; q < R3D_PERF_QUARTER_COUNT; q++) {
            render_lab_test_enter(&saved, scenario->scene->key);
            suite_set_test_cleanup(release_fixture);
            render_lab_test_hold_quarter(r3d_perf_quarters[q].quarter);
            scene_t* scene = render_lab_test_loaded_scene(scenario->pack == NULL ? SPONZA_SCENE : scenario->pack);
            TEST_ASSERT_NOT_NULL(scene);
            const scene_entity_t entity =
                scene_find(scene, name_hash(scenario->entity == NULL ? sponza_cameras[0] : scenario->entity));
            TEST_ASSERT_NOT_EQUAL(SCENE_ENTITY_NONE, entity);
            const capybara_take_t* take = scenario->take;
            const char* row = take == NULL ? scenario->scene->key : take->row;
            if (take != NULL) {
                send(CAPYBARA_VIEW_COMMAND, take->view);
                TEST_ASSERT_TRUE(scene_animator_seek(scene, entity, SCENE_ANIMATOR_CURRENT, 0));
                send(CAPYBARA_CLIP_COMMAND, take->blend_ms);
            }
            TEST_ASSERT_TRUE(scene_animator_seek(scene, entity, SCENE_ANIMATOR_CURRENT, 0));
            const int clip = scene->animators[entity].layer.current.clip;
            const uint32_t duration_ms = take != NULL && take->blend_ms > 0
                                             ? (uint32_t)take->blend_ms
                                             : scene_animator_clip_duration_ms(scene, entity, clip);
            TEST_ASSERT_GREATER_THAN_UINT32(0, duration_ms);
            const frame_profile_scenario_t playback = {row, (int64_t)duration_ms * MATH_US_PER_MS};
            int64_t remainder_us = 0;
            const frame_profile_t p = frame_profile_play(&playback, whole_frame, &remainder_us);
            frame_profile_log("render_lab_profile", row, &p, r3d_perf_quarters[q].name);
            TEST_ASSERT_GREATER_THAN_INT_MESSAGE(0, p.frames, "frame profile failed");
            const perf_budget_t* budget = &scenario->quarters[q];
            r3d_perf_budget(row, &r3d_perf_quarters[q], p.frame_us.avg, p.frame_us.p95, budget, false);
            if (over[0] == '\0' && !perf_guard_passes(p.frame_us.p95, budget->p95_ceiling_us)) {
                const int length = snprintf(over, sizeof over, "%s %s", row, r3d_perf_quarters[q].name);
                TEST_ASSERT_TRUE(length > 0 && (size_t)length < sizeof over);
                over_p95_us = p.frame_us.p95;
                over_ceiling_us = budget->p95_ceiling_us;
            }
            release_fixture();
            suite_clear_test_cleanup();
        }
    }
    if (over[0] != '\0') {
        perf_guard(over, over_p95_us, over_ceiling_us);
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
