/* Device suite: each herd size shows its first N capybaras and hides the
 * rest, every one shown drawn, and what a turn of the camera round them
 * costs: the whole frame, and the animation step (posing and skinning every
 * copy shown) alone, with the internal heap the herd leaves. Logged per size
 * and quarter, no budget yet; the slope over N is the cost of one more
 * skinned copy. */
#include "suites.h"
#ifdef DEVICE_BUILD
#include "apps/render_lab/herd/scene_herd.h"
#include "apps/render_lab/render_lab.h"
#include "core/memory.h"
#include "core/timing.h"
#include "core/tune.h"
#include "esp_log.h"
#include "frame/test_frame.h"
#include "lab/lab_view.h"
#include "lab/test/lab_perf_quarters.h"
#include "lab/test/lab_test.h"
#include "scene/entity/entity.h"
#include "scene/scene_internal.h"
#include "testkit/bench.h"
#include "testkit/cleanup.h"
#include "unity.h"

/* Frames sampled per turn of the camera, spread evenly round it. */
#define HERD_PERF_SAMPLES 100

/* One turn of the camera (render_lab.orbit_ms), and so each sample's step. */
#define HERD_PERF_TURN_MS 18000
#define HERD_PERF_DT_MS   (HERD_PERF_TURN_MS / HERD_PERF_SAMPLES)
_Static_assert(HERD_PERF_TURN_MS <= LAB_ORBIT_MAX_MS, "a turn render_lab.orbit_ms can hold");

static const char* TAG = "herd_perf";

typedef struct {
    lab_test_t saved;
    int32_t frame_us[HERD_PERF_SAMPLES];
    int32_t step_us[HERD_PERF_SAMPLES];
} fixture_t;

static fixture_t* active;
static const lab_scene_t* herd; /* the size the next test shows */

static void
release_fixture(void) {
    (void)tune_reset(tune_shared(), TUNE_NAME(render_lab, orbit_ms));
    lab_test_exit(&active->saved);
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

static void
test_herd_turn_cost(void) {
    active = memory_alloc(sizeof *active, MEMORY_PSRAM);
    TEST_ASSERT_NOT_NULL(active);
    *active = (fixture_t){0};
    const size_t internal_before = memory_free_bytes(MEMORY_INTERNAL);
    lab_test_enter(&active->saved, &render_lab, herd->key);
    suite_set_test_cleanup(release_fixture);
    TEST_ASSERT_TRUE(tune_set(tune_shared(), TUNE_NAME(render_lab, orbit_ms), HERD_PERF_TURN_MS));
    const entity_t root = lab_test_loaded_scene("herd");
    TEST_ASSERT_FALSE(handle_is_none(root));
    scene_row_t row;
    scene_t* scene = scene_entity_row(root, &row);
    TEST_ASSERT_NOT_NULL(scene);
    scene_animation_step(scene, 0); /* a shown copy takes its buffers on its first step */
    const int copies = *(const int*)herd->data;
    TEST_ASSERT_EQUAL_INT(copies, shown_copies(scene));
    TEST_ASSERT_EQUAL_INT(0, scene->starved_count);
    ESP_LOGI(TAG, "herd_perf: n=%d internal_taken=%u internal_free=%u internal_largest=%u", copies,
             (unsigned)(internal_before - memory_free_bytes(MEMORY_INTERNAL)),
             (unsigned)memory_free_bytes(MEMORY_INTERNAL), (unsigned)memory_largest_block(MEMORY_INTERNAL));

    for (int i = -1; i < HERD_PERF_SAMPLES; i++) {
        const int64_t start = timing_now_us();
        scene_animation_step(scene, HERD_PERF_DT_MS);
        if (i >= 0) {
            active->step_us[i] = (int32_t)(timing_now_us() - start);
        }
    }
    const bench_stats_t step = bench_stats_compute(active->step_us, HERD_PERF_SAMPLES);
    TEST_ASSERT_GREATER_THAN_INT64(0, step.avg);
    ESP_LOGI(TAG, "herd_perf: n=%d samples=%d step_mean_us=%lld step_p95_us=%lld", copies, HERD_PERF_SAMPLES,
             (long long)step.avg, (long long)step.p95);

    for (size_t q = 0; q < LAB_PERF_QUARTER_COUNT; q++) {
        const lab_perf_quarter_t* quarter = &lab_perf_quarters[q];
        test_hold_quarter(quarter->quarter);
        for (int i = -1; i < HERD_PERF_SAMPLES; i++) {
            const int64_t start = timing_now_us();
            (void)lab_test_frame_dt(HERD_PERF_DT_MS, false, NULL);
            if (i >= 0) {
                active->frame_us[i] = (int32_t)(timing_now_us() - start);
            }
        }
        TEST_ASSERT_EQUAL_INT(0, scene->starved_count);
        const bench_stats_t frame = bench_stats_compute(active->frame_us, HERD_PERF_SAMPLES);
        TEST_ASSERT_GREATER_THAN_INT64(0, frame.avg);
        ESP_LOGI(TAG, "herd_perf: n=%d samples=%d frame_mean_us=%lld frame_p95_us=%lld" LAB_PERF_QUARTER_FORMAT, copies,
                 HERD_PERF_SAMPLES, (long long)frame.avg, (long long)frame.p95, quarter->name);
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
        RUN_TEST_CASE(test_herd_turn_cost, herd->key);
    }
#endif
}

SUITE_REGISTER(run_herd_perf_suite);
