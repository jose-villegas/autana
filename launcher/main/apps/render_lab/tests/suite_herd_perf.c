/* Device suite: each herd size shows its first N capybaras and hides the
 * rest, every one shown drawn, and what a lap of the camera round them
 * costs: the whole frame, and the animation step (posing and skinning every
 * copy shown) alone, with the internal heap the herd leaves. Logged per size
 * and quarter, no budget yet; the slope over N is the cost of one more
 * skinned copy. */
#include "suites.h"
#ifdef DEVICE_BUILD
#include "apps/render_lab/herd/scene_herd.h"
#include "apps/render_lab/tests/r3d_perf_quarters.h"
#include "apps/render_lab/tests/render_lab_test.h"
#include "core/memory.h"
#include "core/timing.h"
#include "core/tune.h"
#include "esp_log.h"
#include "frame/test_frame.h"
#include "render/context/render_context.h"
#include "scene/animator/animator.h"
#include "scene/entity/entity.h"
#include "scene/scene_internal.h"
#include "scene/transform/transform.h"
#include "testkit/bench.h"
#include "testkit/cleanup.h"
#include "unity.h"

/* Frames sampled per lap of the camera, spread evenly round it. */
#define HERD_PERF_SAMPLES        100
#define HERD_RATE_DT_MS          16
#define HERD_RATE_LAYOUT_COLUMNS 3
#define HERD_RATE_COLUMN_SPACING 0.25F

static const char* TAG = "herd_perf";

typedef struct {
    render_lab_test_t saved;
    int32_t frame_us[HERD_PERF_SAMPLES];
    int32_t step_us[HERD_PERF_SAMPLES];
} fixture_t;

static fixture_t* active;
static const render_lab_scene_t* herd; /* the size the next test shows */

static void
release_fixture(void) {
    (void)tune_reset(tune_shared(), TUNE_NAME(anim, mid_every));
    (void)tune_reset(tune_shared(), TUNE_NAME(anim, far_every));
    render_lab_test_exit(&active->saved);
    memory_free(active);
    active = NULL;
}

/* The skinned copies the scene keeps active. */
static int
shown_copies(const scene_t* scene) {
    int shown = 0;
    for (int e = 0; e < scene->asset.entity_count; e++) {
        shown += scene->skinned_renderers[e].bound && entity_active(scene_row_entity(scene, (scene_row_t)e));
    }
    return shown;
}

static bench_stats_t
sample_herd_steps(scene_t* scene, uint32_t dt_ms) {
    for (int i = -1; i < HERD_PERF_SAMPLES; i++) {
        const int64_t start = timing_now_us();
        scene_animation_step(scene, dt_ms);
        if (i >= 0) {
            active->step_us[i] = (int32_t)(timing_now_us() - start);
        }
    }
    return bench_stats_compute(active->step_us, HERD_PERF_SAMPLES);
}

static bench_stats_t
sample_herd_frames(uint32_t dt_ms) {
    for (int i = -1; i < HERD_PERF_SAMPLES; i++) {
        const int64_t start = timing_now_us();
        (void)render_lab_test_frame_dt(dt_ms, false, NULL);
        if (i >= 0) {
            active->frame_us[i] = (int32_t)(timing_now_us() - start);
        }
    }
    return bench_stats_compute(active->frame_us, HERD_PERF_SAMPLES);
}

static void
test_herd_lap_cost(void) {
    active = memory_alloc(sizeof *active, MEMORY_PSRAM);
    TEST_ASSERT_NOT_NULL(active);
    *active = (fixture_t){0};
    const size_t internal_before = memory_free_bytes(MEMORY_INTERNAL);
    render_lab_test_enter(&active->saved, herd->key);
    suite_set_test_cleanup(release_fixture);
    const entity_t root = render_lab_test_loaded_scene("herd");
    TEST_ASSERT_FALSE(handle_is_none(root));
    scene_row_t row;
    scene_t* scene = scene_entity_row(root, &row);
    TEST_ASSERT_NOT_NULL(scene);
    const render_lab_path_t path = herd->path();
    const uint32_t lap_ms = animator_clip_duration_ms(path.entity, animator_current_clip(path.entity));
    TEST_ASSERT_GREATER_THAN_UINT32(HERD_PERF_SAMPLES, lap_ms);
    const uint32_t dt_ms = lap_ms / HERD_PERF_SAMPLES;
    scene_animation_step(scene, 0); /* a shown copy takes its buffers on its first step */
    const int copies = *(const int*)herd->data;
    TEST_ASSERT_EQUAL_INT(copies, shown_copies(scene));
    TEST_ASSERT_EQUAL_INT(0, scene->starved_count);
    ESP_LOGI(TAG, "herd_perf: n=%d internal_taken=%u internal_free=%u internal_largest=%u", copies,
             (unsigned)(internal_before - memory_free_bytes(MEMORY_INTERNAL)),
             (unsigned)memory_free_bytes(MEMORY_INTERNAL), (unsigned)memory_largest_block(MEMORY_INTERNAL));

    const bench_stats_t step = sample_herd_steps(scene, dt_ms);
    TEST_ASSERT_GREATER_THAN_INT64(0, step.avg);
    ESP_LOGI(TAG, "herd_perf: n=%d samples=%d step_mean_us=%lld step_p95_us=%lld", copies, HERD_PERF_SAMPLES,
             (long long)step.avg, (long long)step.p95);

    for (size_t q = 0; q < R3D_PERF_QUARTER_COUNT; q++) {
        const r3d_perf_quarter_t* quarter = &r3d_perf_quarters[q];
        test_hold_quarter(quarter->quarter);
        TEST_ASSERT_TRUE(animator_seek(path.entity, ANIMATOR_CURRENT, 0));
        const bench_stats_t frame = sample_herd_frames(dt_ms);
        TEST_ASSERT_EQUAL_INT(0, scene->starved_count);
        TEST_ASSERT_GREATER_THAN_INT64(0, frame.avg);
        ESP_LOGI(TAG, "herd_perf: n=%d samples=%d frame_mean_us=%lld frame_p95_us=%lld" R3D_PERF_QUARTER_FORMAT, copies,
                 HERD_PERF_SAMPLES, (long long)frame.avg, (long long)frame.p95, quarter->name);
    }
    suite_clear_test_cleanup();
    release_fixture();
}

static void
place_rate_herd(scene_t* scene, entity_t camera) {
    const transformf_t eye = transform_world(camera);
    const render_view_t* view = &render_context_main()->kept_view;
    const float radii[] = {RENDER_ANIM_MID_PX * 2.0F, (RENDER_ANIM_MID_PX + RENDER_ANIM_FAR_PX) / 2.0F,
                           RENDER_ANIM_FAR_PX / 2.0F};
    int copy = 0;
    for (int e = 0; e < scene->asset.entity_count; e++) {
        if (!scene->skinned_renderers[e].bound || !(scene->flags[e] & SCENE_FLAG_ACTIVE)) {
            continue;
        }
        const entity_t entity = scene_row_entity(scene, (scene_row_t)e);
        transformf_t pose = transform_world(entity);
        const int renderer = scene_renderer_index(scene, (scene_row_t)e);
        TEST_ASSERT_GREATER_OR_EQUAL_INT(0, renderer);
        const box3f_t bounds = scene->renderers[renderer].world_bounds;
        const vec3f_t extent = box3f_half_extent(bounds);
        const float radius = mathf_sqrt(vec3f_dot(extent, extent));
        const float depth = radius * (1.0F + view->lens.fx / radii[copy % HERD_RATE_LAYOUT_COLUMNS]);
        const float column = (float)(copy % HERD_RATE_LAYOUT_COLUMNS - 1) * HERD_RATE_COLUMN_SPACING;
        const float row = (float)(copy / HERD_RATE_LAYOUT_COLUMNS - 1) * HERD_RATE_COLUMN_SPACING;
        const vec3f_t offset = quatf_rotate(eye.rotation, (vec3f_t){depth * column, depth * row, depth});
        pose.position = vec3f_sub(vec3f_add(eye.position, offset), vec3f_sub(box3f_centre(bounds), pose.position));
        transform_set_world(entity, pose);
        copy++;
    }
}

static void
test_herd_fixed_animation_rates(void) {
    active = memory_alloc(sizeof *active, MEMORY_PSRAM);
    TEST_ASSERT_NOT_NULL(active);
    *active = (fixture_t){0};
    render_lab_test_enter(&active->saved, scene_herd[HERD_EIGHT].key);
    suite_set_test_cleanup(release_fixture);
    const entity_t root = render_lab_test_loaded_scene("herd");
    scene_row_t row;
    scene_t* scene = scene_entity_row(root, &row);
    TEST_ASSERT_NOT_NULL(scene);
    const render_lab_path_t path = scene_herd[HERD_EIGHT].path();
    TEST_ASSERT_TRUE(animator_seek(path.entity, ANIMATOR_CURRENT, 0));
    TEST_ASSERT_TRUE(animator_set_paused(path.entity, ANIMATOR_CURRENT, true));
    (void)render_lab_test_frame_dt(0, false, NULL);
    place_rate_herd(scene, path.entity);
    (void)render_lab_test_frame_dt(0, false, NULL);
    for (int arm = 0; arm < 2; arm++) {
        const char* name = arm == 0 ? "off" : "tiered";
        TEST_ASSERT_TRUE(tune_set(tune_shared(), TUNE_NAME(anim, mid_every),
                                  arm == 0 ? RENDER_ANIM_EVERY_MIN : RENDER_ANIM_MID_EVERY));
        TEST_ASSERT_TRUE(tune_set(tune_shared(), TUNE_NAME(anim, far_every),
                                  arm == 0 ? RENDER_ANIM_EVERY_MIN : RENDER_ANIM_FAR_EVERY));
        scene_animation_step(scene, 0);
        const int intervals[] = {RENDER_ANIM_EVERY_MIN, RENDER_ANIM_MID_EVERY, RENDER_ANIM_FAR_EVERY};
        int copy = 0;
        for (int e = 0; e < scene->asset.entity_count; e++) {
            if (scene->skinned_renderers[e].bound && (scene->flags[e] & SCENE_FLAG_ACTIVE)) {
                TEST_ASSERT_EQUAL_INT(arm == 0 ? RENDER_ANIM_EVERY_MIN : intervals[copy % HERD_RATE_LAYOUT_COLUMNS],
                                      scene->animators[e].interval);
                copy++;
            }
        }
        const bench_stats_t step = sample_herd_steps(scene, HERD_RATE_DT_MS);
        ESP_LOGI(TAG, "herd_perf: n=%d anim=%s samples=%d step_mean_us=%lld step_p95_us=%lld step_max_us=%lld",
                 *(const int*)scene_herd[HERD_EIGHT].data, name, HERD_PERF_SAMPLES, (long long)step.avg,
                 (long long)step.p95, (long long)step.max);
        for (size_t q = 0; q < R3D_PERF_QUARTER_COUNT; q++) {
            const r3d_perf_quarter_t* quarter = &r3d_perf_quarters[q];
            test_hold_quarter(quarter->quarter);
            const bench_stats_t frame = sample_herd_frames(HERD_RATE_DT_MS);
            TEST_ASSERT_EQUAL_INT(0, scene->starved_count);
            ESP_LOGI(TAG,
                     "herd_perf: n=%d anim=%s samples=%d frame_mean_us=%lld frame_p95_us=%lld" R3D_PERF_QUARTER_FORMAT,
                     *(const int*)scene_herd[HERD_EIGHT].data, name, HERD_PERF_SAMPLES, (long long)frame.avg,
                     (long long)frame.p95, quarter->name);
        }
    }
    suite_clear_test_cleanup();
    release_fixture();
}

#endif

void
run_herd_perf_suite(void) {
#ifdef DEVICE_BUILD
    RUN_TEST(test_herd_fixed_animation_rates);
    for (int i = 0; i < HERD_SIZE_COUNT; i++) {
        herd = &scene_herd[i];
        RUN_TEST_CASE(test_herd_lap_cost, herd->key);
    }
#endif
}

SUITE_REGISTER(run_herd_perf_suite);
