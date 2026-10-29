/* On request: what a generated frame costs on the board (render/framegen,
 * docs/render/Frame-Generation.md). Each scenario's path is drawn at every
 * half-picture ladder step; at poses along it the drawn frame is warped one
 * display step on and filled, as the render context does with frame
 * generation on. The present is the frame's own half-picture expand. */
#include "suites.h"
#ifdef DEVICE_BUILD
#include "apps/render_lab/baked_families.h"
#include "apps/render_lab/render_lab_dynres.h"
#include "apps/render_lab/render_lab_scene.h"
#include "apps/render_lab/render_lab_view.h"
#include "apps/render_lab/scene_baked.h"
#include "apps/render_lab/sponza_content.h"
#include "apps/render_lab/tests/r3d_perf_quarters.h"
#include "apps/render_lab/tests/render_lab_test.h"
#include "core/timing.h"
#include "esp_log.h"
#include "gfx/gfx.h"
#include "render/context/render_context.h"
#include "render/framegen/framegen.h"
#include "scene/camera/camera.h"
#include "scene/scene_internal.h"
#include "testkit/cleanup.h"
#include "unity.h"

static const char* TAG = "framegen";

/* How far ahead a generated frame looks: one displayed frame. */
#define FRAMEGEN_AHEAD_MS RENDER_LAB_FRAME_TARGET_MS
/* Poses timed along each path. */
#define FRAMEGEN_POSES    24
#define FRAMEGEN_PERCENT  100
#define FRAMEGEN_LINE     "framegen: %s %dx%d poses=%d frame_us=%d present_us=%d warp_us=%d/%d fill_us=%d holes_pct=%d.%02d"

typedef struct {
    const render_lab_scene_t* scene;
    const char* pack;
    const char* camera;
} framegen_scenario_t;

static const framegen_scenario_t scenarios[] = {
    {&scene_bistro[SCENE_BAKED_FITTED_FULL], BISTRO_SCENE, BISTRO_CAMERA},
    {&scene_sponza[SCENE_BAKED_FITTED_FULL], SPONZA_SCENE, NULL},
};

static render_lab_test_t saved;
static const framegen_scenario_t* scenario;
static int ladder_step;
static framegen_target_t target;

static void
release_fixture(void) {
    framegen_target_free(&target);
    render_lab_test_exit(&saved);
}

/* The view through the camera at `t_ms` along its path, as the scene draws it. */
static render_view_t
view_at(scene_t* scene, scene_entity_t camera, const camera_t* lens, uint32_t t_ms) {
    TEST_ASSERT_TRUE(scene_animator_seek(scene, camera, SCENE_ANIMATOR_CURRENT, t_ms));
    const transformf_t pose = scene_entity_world(scene, camera);
    return camera_view(lens, &pose, (viewport_t){GFX_WIDTH, GFX_HEIGHT, r3d_perf_quarters[0].quarter});
}

typedef struct {
    int64_t frame, present, warp, warp_max, fill, holes, pixels;
} totals_t;

/* The test's sums; static, so the deep frame draw below has the stack to itself. */
static totals_t sum;

/* The frame drawn at `t_ms` warped one display step on, timed. Not inlined: its
 * views are off the stack while the frame draws. */
static __attribute__((noinline)) void
time_warp(scene_t* scene, scene_entity_t camera, const camera_t* lens, uint32_t t_ms) {
    const raster_t* raster = &render_context_main()->raster;
    const render_view_t now = view_at(scene, camera, lens, t_ms);
    const render_view_t next = view_at(scene, camera, lens, t_ms + FRAMEGEN_AHEAD_MS);
    const int64_t start = timing_now_us();
    framegen_warp(raster, &now, &next, &target);
    const int64_t warped = timing_now_us();
    sum.holes += framegen_fill(raster, &target);
    sum.fill += timing_now_us() - warped;
    sum.warp += warped - start;
    sum.warp_max = warped - start > sum.warp_max ? warped - start : sum.warp_max;
    sum.pixels += (int64_t)raster->width * raster->height;
}

static void
test_generated_frame_cost(void) {
    render_lab_test_enter(&saved, scenario->scene->key);
    suite_set_test_cleanup(release_fixture);
    render_lab_test_hold_quarter(r3d_perf_quarters[0].quarter);
    scene_t* scene = render_lab_test_loaded_scene(scenario->pack);
    TEST_ASSERT_NOT_NULL(scene);
    const char* camera_name = scenario->camera == NULL ? sponza_cameras[0] : scenario->camera;
    const scene_entity_t camera = scene_find(scene, name_hash(camera_name));
    TEST_ASSERT_NOT_EQUAL(SCENE_ENTITY_NONE, camera);
    const camera_t* lens = scene_camera_lens(scene, name_hash(camera_name));
    TEST_ASSERT_NOT_NULL(lens);
    TEST_ASSERT_TRUE(scene_animator_seek(scene, camera, SCENE_ANIMATOR_CURRENT, 0));
    const uint32_t duration_ms =
        scene_animator_clip_duration_ms(scene, camera, scene->animators[camera].layer.current.clip);
    TEST_ASSERT_GREATER_THAN_UINT32(FRAMEGEN_AHEAD_MS, duration_ms);

    const resolution_step_t size = render_lab_ladder[ladder_step];
    const resolution_config_t pinned = resolution_config(&size, 1, 1, RENDER_LAB_BUDGET_MAX_MS * MATH_US_PER_MS);
    render_context_set_dynamic_resolution(render_context_main(), &pinned, NULL, 0);
    TEST_ASSERT_TRUE(framegen_target_alloc(&target, size.width, size.height));
    (void)render_lab_test_frame_dt(0, false, NULL); /* warm-up, not sampled */

    sum = (totals_t){0};
    for (int i = 0; i < FRAMEGEN_POSES; i++) {
        const uint32_t t_ms = (uint32_t)(((uint64_t)(duration_ms - FRAMEGEN_AHEAD_MS) * (uint64_t)i) / FRAMEGEN_POSES);
        TEST_ASSERT_TRUE(scene_animator_seek(scene, camera, SCENE_ANIMATOR_CURRENT, t_ms));
        const int64_t start = timing_now_us();
        (void)render_lab_test_frame_dt(0, false, NULL);
        sum.frame += timing_now_us() - start;
        sum.present += render_context_frame(render_context_main()).upscale_us;
        TEST_ASSERT_EQUAL_INT(size.width, render_context_main()->raster.width);
        TEST_ASSERT_EQUAL_INT(size.height, render_context_main()->raster.height);
        time_warp(scene, camera, lens, t_ms);
    }
    const int64_t hundredths = (sum.holes * FRAMEGEN_PERCENT * FRAMEGEN_PERCENT) / sum.pixels;
    ESP_LOGI(TAG, FRAMEGEN_LINE, scenario->scene->key, size.width, size.height, FRAMEGEN_POSES,
             (int)(sum.frame / FRAMEGEN_POSES), (int)(sum.present / FRAMEGEN_POSES), (int)(sum.warp / FRAMEGEN_POSES),
             (int)sum.warp_max, (int)(sum.fill / FRAMEGEN_POSES), (int)(hundredths / FRAMEGEN_PERCENT),
             (int)(hundredths % FRAMEGEN_PERCENT));
    release_fixture();
    suite_clear_test_cleanup();
}
#endif

/* Every scenario at every half-picture ladder step, portrait. */
void
run_framegen_perf_suite(void) {
#ifdef DEVICE_BUILD
    for (size_t i = 0; i < sizeof scenarios / sizeof scenarios[0]; i++) {
        scenario = &scenarios[i];
        for (ladder_step = 0; ladder_step < RENDER_LAB_LADDER_STEPS; ladder_step++) {
            if (render_lab_ladder[ladder_step].width * 2 == GFX_WIDTH) {
                RUN_TEST(test_generated_frame_cost);
            }
        }
    }
#endif
}

SUITE_REGISTER_ON_REQUEST(run_framegen_perf_suite);
