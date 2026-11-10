/*
 * The stage costs of each stress preset, on the board only: poses evenly
 * along the preset's camera swing, each replayed paused as the frame profile
 * replays its poses, so every line carries the cull view's counts and the
 * r3d.cull, transform, draw and upscale times in the frame profile's format.
 * Quick on purpose: one quarter and no real-time play, so all the presets
 * time in well under two minutes and an A/B of a render change can run them
 * before the full frame profile confirms it. Logged only; nothing is pegged.
 */

#include "suites.h"
#ifdef DEVICE_BUILD
#include "apps/render_lab/stress/scene_stress.h"
#include "apps/render_lab/tests/r3d_perf_quarters.h"
#include "apps/render_lab/tests/render_lab_test.h"
#include "core/memory.h"
#include "frame/test_frame.h"
#include "render/context/render_context.h"
#include "scene/animator/animator.h"
#include "testkit/cleanup.h"
#include "unity.h"

static const char* TAG = "stress_perf";

/* The stress pack's scene id and its swinging camera (scene_stress.c). */
#define STRESS_PERF_PACK   "stress"
#define STRESS_PERF_CAMERA "camera"

/* Poses replayed evenly along each swing. */
#define STRESS_PERF_POSES  16

static render_lab_test_t saved;
static render_lab_test_pose_t* pose_line; /* too big for the test's stack */
static const render_lab_scene_t* preset;

static void
release_fixture(void) {
    memory_free(pose_line);
    pose_line = NULL;
    render_lab_test_show_view(RENDER_DEBUG_VIEW_SHADED);
    render_lab_test_exit(&saved);
}

static void
test_stress_stages(void) {
    const r3d_perf_quarter_t* quarter = &r3d_perf_quarters[0];
    render_lab_test_enter(&saved, preset->key);
    suite_set_test_cleanup(release_fixture);
    test_hold_quarter(quarter->quarter);
    const entity_t scene = render_lab_test_loaded_scene(STRESS_PERF_PACK);
    TEST_ASSERT_FALSE_MESSAGE(handle_is_none(scene), "the stress scene did not load");
    const entity_t camera = entity_find(scene, name_hash(STRESS_PERF_CAMERA));
    TEST_ASSERT_FALSE(handle_is_none(camera));
    TEST_ASSERT_TRUE(animator_seek(camera, ANIMATOR_CURRENT, 0));
    const uint32_t duration_ms = animator_clip_duration_ms(camera, animator_current_clip(camera));
    TEST_ASSERT_GREATER_THAN_UINT32(0, duration_ms);
    pose_line = memory_alloc(sizeof *pose_line, MEMORY_INTERNAL);
    TEST_ASSERT_NOT_NULL(pose_line);
    for (int i = 0; i < STRESS_PERF_POSES; i++) {
        render_lab_test_replay_pose(TAG, camera, duration_ms, (int32_t)((int64_t)duration_ms * i / STRESS_PERF_POSES),
                                    preset->key, quarter->name, pose_line);
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
        RUN_TEST_CASE(test_stress_stages, preset->key, r3d_perf_quarters[0].name);
    }
#endif
}

SUITE_REGISTER(run_stress_perf_suite);
