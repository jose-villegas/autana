#include "suites.h"
#ifdef DEVICE_BUILD
#include <math.h>
#include <string.h>
#include "apps/render_lab/render_lab_scene.h"
#include "apps/render_lab/scene_water.h"
#include "apps/render_lab/tests/render_lab_test.h"
#include "apps/render_lab/tests/water_perf_log.h"
#include "core/memory.h"
#include "core/timing.h"
#include "core/tune.h"
#include "esp_log.h"
#include "profile/frame_cost.h"
#include "testkit/bench.h"
#include "testkit/cleanup.h"
#include "unity.h"

static const char* TAG = "water_perf";

/* One setting of the experiments' tunes; 0 rest_um_s never rests. */
typedef struct {
    const char* name;
    int32_t grid_internal;
    int32_t substeps;
    int32_t rest_um_s;
    int32_t two_core;
    int32_t red_black;
    int32_t plane;
} water_config_t;

/* The default, then one change each: on one core, other substep counts,
 * the pressure in red-black order on both cores, and skipping the step at
 * rest below 2 mm/s; and method C, the plane, against them. The grid in
 * internal RAM measured the same as in PSRAM, so it is not a row. */
static const water_config_t configs[] = {
    {.name = "base", .substeps = 2, .two_core = 1},
    {.name = "one_core", .substeps = 2},
    {.name = "sub4", .substeps = 4, .two_core = 1},
    {.name = "sub1", .substeps = 1, .two_core = 1},
    {.name = "red_black", .substeps = 2, .two_core = 1, .red_black = 1},
    {.name = "rest", .substeps = 2, .rest_um_s = 2000, .two_core = 1},
    {.name = "plane", .substeps = 2, .two_core = 1, .plane = 1},
};

static const char* const tunes[] = {TUNE_NAME(water, grid_internal), TUNE_NAME(water, substeps),
                                    TUNE_NAME(water, rest_um_s),     TUNE_NAME(water, two_core),
                                    TUNE_NAME(water, red_black),     TUNE_NAME(water, plane)};

/* A row's frames: whole frame, update and draw, in us. */
typedef struct {
    int32_t* frame_us;
    int32_t* update_us;
    int32_t* draw_us;
    int frames;
    scene_water_stats_t sums;
} water_row_t;

static render_lab_test_t saved;
static water_row_t row;
static char* report;

static void
release_fixture(void) {
    render_lab_test_exit(&saved);
    for (size_t i = 0; i < sizeof tunes / sizeof tunes[0]; i++) {
        (void)tune_reset(tune_shared(), tunes[i]);
    }
    memory_free(row.frame_us);
    memory_free(row.update_us);
    memory_free(row.draw_us);
    memory_free(report);
    row = (water_row_t){0};
    report = NULL;
}

static void
heap(const char* config, const char* phase) {
    ESP_LOGI(TAG, WATER_PERF_PREFIX " " WATER_PERF_HEAP, config, phase, (unsigned)memory_free_bytes(MEMORY_INTERNAL),
             (unsigned)memory_largest_block(MEMORY_INTERNAL));
}

static void
add(scene_water_stats_t* sums, const scene_water_stats_t* s) {
    sums->updates += s->updates;
    sums->substeps += s->substeps;
    sums->rested += s->rested;
    sums->update_us += s->update_us;
    for (int i = 0; i < SCENE_WATER_STAGE_COUNT; i++) {
        sums->stage_us[i] += s->stage_us[i];
    }
    for (int i = 0; i < FLIP3_STAGE_COUNT; i++) {
        sums->flip3_us[i] += s->flip3_us[i];
    }
    sums->triangles += s->triangles;
    sums->particles = s->particles;
    sums->peak_speed_mm_s = fmaxf(sums->peak_speed_mm_s, s->peak_speed_mm_s);
}

static void
log_row(const char* config, const char* phase) {
    const int n = row.frames;
    TEST_ASSERT_GREATER_THAN_INT(0, n);
    const bench_stats_t frame = bench_stats_compute(row.frame_us, n);
    const bench_stats_t update = bench_stats_compute(row.update_us, n);
    const bench_stats_t draw = bench_stats_compute(row.draw_us, n);
    const scene_water_stats_t* s = &row.sums;
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(n, s->updates, "the water scene did not update every frame");
    ESP_LOGI(TAG, WATER_PERF_PREFIX " " WATER_PERF_ROW, config, phase, n, (unsigned)s->substeps, (unsigned)s->rested,
             (long long)(s->triangles / n), s->particles, (double)s->peak_speed_mm_s);
    ESP_LOGI(TAG, WATER_PERF_PREFIX " " WATER_PERF_TIMES, config, phase, (long long)frame.avg, (long long)frame.p95,
             (long long)frame.max, (long long)update.avg, (long long)update.p95, (long long)update.max,
             (long long)draw.avg, (long long)draw.p95, (long long)draw.max);
    for (int i = 0; i < SCENE_WATER_STAGE_COUNT; i++) {
        ESP_LOGI(TAG, WATER_PERF_PREFIX " " WATER_PERF_STAGE, config, phase, WATER_PERF_SCENE,
                 scene_water_stage_names[i], (long long)(s->stage_us[i] / n));
    }
    for (int i = 0; i < FLIP3_STAGE_COUNT; i++) {
        ESP_LOGI(TAG, WATER_PERF_PREFIX " " WATER_PERF_STAGE, config, phase, WATER_PERF_FLIP3, flip3_stage_names[i],
                 (long long)(s->flip3_us[i] / n));
    }
    (void)frame_cost_take_report((uint32_t)n, report, FRAME_COST_REPORT_MAX);
    ESP_LOGI(TAG, WATER_PERF_PREFIX " " WATER_PERF_COST, config, phase, report);
    row.frames = 0;
    row.sums = (scene_water_stats_t){0};
}

/* The tour once at a fixed frame time, one row per phase. */
static void
play(const water_config_t* config) {
    const int32_t values[] = {config->grid_internal, config->substeps,  config->rest_um_s,
                              config->two_core,      config->red_black, config->plane};
    _Static_assert(sizeof values / sizeof values[0] == sizeof tunes / sizeof tunes[0], "a value per tune");
    for (size_t i = 0; i < sizeof tunes / sizeof tunes[0]; i++) {
        TEST_ASSERT_TRUE(tune_set(tune_shared(), tunes[i], values[i]));
    }
    heap(config->name, WATER_PERF_BEFORE);
    render_lab_test_enter(&saved, scene_water.key);
    suite_set_test_cleanup(release_fixture);
    TEST_ASSERT_TRUE(app_render_lab.console->handle("water tour"));
    const int capacity = (int)(scene_water_tour_length_ms() / WATER_PERF_FRAME_MS);
    row.frame_us = memory_alloc((size_t)capacity * sizeof(int32_t), MEMORY_PSRAM);
    row.update_us = memory_alloc((size_t)capacity * sizeof(int32_t), MEMORY_PSRAM);
    row.draw_us = memory_alloc((size_t)capacity * sizeof(int32_t), MEMORY_PSRAM);
    report = memory_alloc(FRAME_COST_REPORT_MAX, MEMORY_INTERNAL);
    TEST_ASSERT_TRUE(row.frame_us != NULL && row.update_us != NULL && row.draw_us != NULL && report != NULL);
    /* Untimed: the tour's upright refill. */
    (void)render_lab_test_frame_dt(0, false, NULL);
    (void)scene_water_take_stats();
    (void)frame_cost_take_report(0, report, 0);
    heap(config->name, WATER_PERF_RUNNING);
    const char* phase = scene_water_tour_phase();
    for (int f = 0; f < capacity; f++) {
        const int64_t start = timing_now_us();
        const int64_t draw_us = render_lab_test_frame_dt(WATER_PERF_FRAME_MS, false, NULL);
        const int64_t frame_us = timing_now_us() - start;
        const scene_water_stats_t s = scene_water_take_stats();
        const char* now = scene_water_tour_phase();
        if (strcmp(now, phase) != 0) {
            log_row(config->name, phase);
            phase = now;
        }
        row.frame_us[row.frames] = (int32_t)frame_us;
        row.update_us[row.frames] = (int32_t)s.update_us;
        row.draw_us[row.frames] = (int32_t)draw_us;
        row.frames++;
        add(&row.sums, &s);
    }
    log_row(config->name, phase);
    release_fixture();
    suite_clear_test_cleanup();
    heap(config->name, WATER_PERF_AFTER);
}

static void
test_water_tour_stages(void) {
    for (size_t i = 0; i < sizeof configs / sizeof configs[0]; i++) {
        play(&configs[i]);
    }
}
#endif

void
run_water_perf_suite(void) {
#ifdef DEVICE_BUILD
    RUN_TEST(test_water_tour_stages);
#endif
}

SUITE_REGISTER(run_water_perf_suite);
