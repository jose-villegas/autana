/*
 * The stage costs of each stress preset, on the board only: each preset's
 * own count of poses evenly along its camera swing, replayed paused as the
 * frame profile replays its poses, in both quarters (the uniform presets at
 * fixed render sizes too). Every line carries the cull view's counts and
 * the r3d stage times in the frame profile's format. Quick on purpose, so
 * an A/B of a render change can run it before the full frame profile
 * confirms it. Logged only; nothing is pegged.
 */

#include "suites.h"
#ifdef DEVICE_BUILD
#include <limits.h>
#include "apps/render_lab/render_lab_dynres.h"
#include "apps/render_lab/render_lab_view.h"
#include "apps/render_lab/stress/scene_stress.h"
#include "apps/render_lab/tests/r3d_perf_quarters.h"
#include "apps/render_lab/tests/render_lab_test.h"
#include "core/tune.h"
#include "frame/test_frame.h"
#include "render/context/render_context.h"
#include "scene/animator/animator.h"
#include "testkit/cleanup.h"
#include "unity.h"

static const char* TAG = "stress_perf";

/* Additional sizes use the ladder's full-width and half-height steps. */
#define STRESS_PERF_WIDE_STEP 3

static render_lab_test_t saved;
static const render_lab_scene_t* preset;
static const r3d_perf_quarter_t* quarter;
static int size_step;
static int saved_filter;
static int saved_layout;

static void
release_fixture(void) {
    render_lab_test_show_view(RENDER_DEBUG_VIEW_SHADED);
    render_context_set_dynamic_resolution(render_context_main(), NULL, NULL, 0);
    TEST_ASSERT_TRUE(tune_set(tune_shared(), TUNE_NAME(render_lab, upscale), saved_filter));
    render_lab_test_exit(&saved);
    TEST_ASSERT_TRUE(tune_set(tune_shared(), TUNE_NAME(render_lab, layout), saved_layout));
}

static void
test_stress_stages(void) {
    saved_filter = render_lab_upscale_selected();
    saved_layout = render_lab_layout();
    const stress_params_t* params = preset->data;
    const int layout =
        params->surface == STRESS_SURFACE_UNIFORM && size_step < 0 ? RENDER_LAB_LAYOUT_DOUBLE : RENDER_LAB_LAYOUT_FULL;
    TEST_ASSERT_TRUE(tune_set(tune_shared(), TUNE_NAME(render_lab, layout), layout));
    render_lab_test_enter(&saved, preset->key);
    suite_set_test_cleanup(release_fixture);
    test_hold_quarter(quarter->quarter);
    (void)render_lab_test_frame_dt(0, false, NULL);
    if (size_step >= 0) {
        const resolution_step_t size = render_lab_ladder[size_step];
        const resolution_config_t one = resolution_config(&size, 1, 1, INT32_MAX);
        render_context_set_dynamic_resolution(render_context_main(), &one, NULL, 0);
        TEST_ASSERT_TRUE(tune_set(tune_shared(), TUNE_NAME(render_lab, upscale), UPSCALE_FILTER_SURFACE));
    }
    const render_lab_path_t path = preset->path();
    TEST_ASSERT_FALSE_MESSAGE(handle_is_none(path.scene), "the stress scene did not load");
    const entity_t camera = path.entity;
    TEST_ASSERT_FALSE(handle_is_none(camera));
    TEST_ASSERT_TRUE(animator_seek(camera, ANIMATOR_CURRENT, 0));
    const uint32_t duration_ms = animator_clip_duration_ms(camera, animator_current_clip(camera));
    TEST_ASSERT_GREATER_THAN_UINT32(0, duration_ms);
    for (int i = 0; i < params->perf_poses; i++) {
        render_lab_test_replay_pose(TAG, camera, duration_ms, (int32_t)((int64_t)duration_ms * i / params->perf_poses),
                                    preset->key, quarter->name);
    }
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
        const int sizes[] = {-1, 0, STRESS_PERF_WIDE_STEP};
        const int size_count = params->surface == STRESS_SURFACE_UNIFORM ? (int)(sizeof sizes / sizeof sizes[0]) : 1;
        for (int q = 0; q < (int)R3D_PERF_QUARTER_COUNT; q++) {
            quarter = &r3d_perf_quarters[q];
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
