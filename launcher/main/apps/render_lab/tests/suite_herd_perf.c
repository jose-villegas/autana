/* Device suite: each herd size shows its first N capybaras and hides the
 * rest, and what a lap of it costs: the whole frame and the animation step
 * (posing and skinning every copy shown), with the internal heap the herd
 * leaves. Logged per size and quarter, no budget yet; the slope over N is the
 * cost of one more skinned copy. */
#include "suites.h"
#ifdef DEVICE_BUILD
#include "apps/render_lab/herd/scene_herd.h"
#include "apps/render_lab/tests/r3d_perf_quarters.h"
#include "apps/render_lab/tests/render_lab_test.h"
#include "core/memory.h"
#include "core/timing.h"
#include "esp_log.h"
#include "frame/test_frame.h"
#include "scene/scene_internal.h"
#include "testkit/bench.h"
#include "testkit/cleanup.h"
#include "unity.h"

/* Frames sampled per lap, spread evenly round it. */
#define HERD_PERF_SAMPLES 100

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
    render_lab_test_exit(&active->saved);
    memory_free(active);
    active = NULL;
}

/* The copies drawn: every skinned renderer the scene keeps active. */
static int
copies_shown(const scene_t* scene) {
    int shown = 0;
    for (int e = 0; e < scene->asset.entity_count; e++) {
        shown += scene->skinned_renderers[e].internal_storage != NULL && scene_entity_active(scene, e);
    }
    return shown;
}

static void
test_herd_lap_cost(void) {
    active = memory_alloc(sizeof *active, MEMORY_PSRAM);
    TEST_ASSERT_NOT_NULL(active);
    *active = (fixture_t){0};
    const size_t internal_before = memory_free_bytes(MEMORY_INTERNAL);
    render_lab_test_enter(&active->saved, herd->key);
    suite_set_test_cleanup(release_fixture);
    scene_t* scene = render_lab_test_loaded_scene("herd");
    TEST_ASSERT_NOT_NULL(scene);
    const int copies = *(const int*)herd->data;
    TEST_ASSERT_EQUAL_INT(copies, copies_shown(scene));
    ESP_LOGI(TAG, "herd_perf: n=%d internal_taken=%u internal_free=%u internal_largest=%u", copies,
             (unsigned)(internal_before - memory_free_bytes(MEMORY_INTERNAL)),
             (unsigned)memory_free_bytes(MEMORY_INTERNAL), (unsigned)memory_largest_block(MEMORY_INTERNAL));

    const render_lab_path_t path = herd->path();
    const int lap_clip = scene_animator_current_clip(path.scene, path.entity);
    const uint32_t lap_ms = scene_animator_clip_duration_ms(path.scene, path.entity, lap_clip);
    TEST_ASSERT_GREATER_THAN_UINT32(HERD_PERF_SAMPLES, lap_ms);
    const uint32_t dt_ms = lap_ms / HERD_PERF_SAMPLES;
    for (size_t q = 0; q < R3D_PERF_QUARTER_COUNT; q++) {
        const r3d_perf_quarter_t* quarter = &r3d_perf_quarters[q];
        test_hold_quarter(quarter->quarter);
        TEST_ASSERT_TRUE(scene_animator_seek(path.scene, path.entity, SCENE_ANIMATOR_CURRENT, 0));
        for (int i = -1; i < HERD_PERF_SAMPLES; i++) {
            const int64_t step_start = timing_now_us();
            scene_animation_step(scene, RENDER_LAB_TEST_FRAME_MS);
            const int64_t step_us = timing_now_us() - step_start;
            const int64_t frame_start = timing_now_us();
            (void)render_lab_test_frame_dt(dt_ms, false, NULL);
            if (i >= 0) {
                active->step_us[i] = (int32_t)step_us;
                active->frame_us[i] = (int32_t)(timing_now_us() - frame_start);
            }
        }
        const bench_stats_t frame = bench_stats_compute(active->frame_us, HERD_PERF_SAMPLES);
        const bench_stats_t step = bench_stats_compute(active->step_us, HERD_PERF_SAMPLES);
        TEST_ASSERT_GREATER_THAN_INT64(0, frame.avg);
        ESP_LOGI(TAG,
                 "herd_perf: n=%d samples=%d frame_mean_us=%lld frame_p95_us=%lld step_mean_us=%lld "
                 "step_p95_us=%lld" R3D_PERF_QUARTER_FORMAT,
                 copies, HERD_PERF_SAMPLES, (long long)frame.avg, (long long)frame.p95, (long long)step.avg,
                 (long long)step.p95, quarter->name);
    }
    suite_clear_test_cleanup();
    release_fixture();
}
#endif

void
run_herd_perf_suite(void) {
#ifdef DEVICE_BUILD
    for (int i = 0; i < HERD_SIZE_COUNT; i++) {
        herd = &scene_herd[i];
        RUN_TEST_CASE(test_herd_lap_cost, herd->key);
    }
#endif
}

SUITE_REGISTER(run_herd_perf_suite);
