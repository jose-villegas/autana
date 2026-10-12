#include "suites.h"
#ifdef DEVICE_BUILD
#include <stdio.h>
#include <string.h>
#include "apps/render_lab/tests/capybara_perf_log.h"
#include "apps/render_lab/tests/render_lab_test.h"
#include "core/job.h"
#include "core/memory.h"
#include "core/timing.h"
#include "esp_cpu.h"
#include "esp_log.h"
#include "perf_stats.h"
#include "scene/scene_internal.h"
#include "test_cleanup.h"
#include "unity.h"

#define PERF_SAMPLES  100
#define CLIP_BLEND_MS 300
#define CLIP_CUT_MS   0
#define CLIP_COMMAND  "clip gallop %d"

static const char* TAG = "capybara_perf";

typedef struct {
    render_lab_test_t saved;
    scene_t* scene;
    scene_entity_t subject;
    int32_t elapsed_us[PERF_SAMPLES];
    int32_t render_us[PERF_SAMPLES];
    int32_t cpu_cycles[PERF_SAMPLES];
    void* positions;
    size_t position_bytes;
} fixture_t;

static fixture_t* active;

static void
heap_report(const char* phase) {
    ESP_LOGI(TAG, CAPYBARA_PERF_PREFIX " phase=%s internal_free=%u internal_largest=%u", phase,
             (unsigned)memory_free_bytes(MEMORY_INTERNAL), (unsigned)memory_largest_block(MEMORY_INTERNAL));
}

static void
release_fixture(void) {
    render_lab_test_exit(&active->saved);
    memory_free(active->positions);
    memory_free(active);
    active = NULL;
}

static fixture_t*
fixture(void) {
    active = memory_alloc(sizeof *active, MEMORY_PSRAM);
    TEST_ASSERT_NOT_NULL(active);
    *active = (fixture_t){0};
    heap_report(CAPYBARA_PERF_PHASE_BEFORE_ENTER);
    render_lab_test_enter(&active->saved, "capybara");
    suite_set_test_cleanup(release_fixture);
    active->scene = render_lab_test_loaded_scene("capybara");
    TEST_ASSERT_NOT_NULL(active->scene);
    active->subject = scene_find(active->scene, "capybara");
    TEST_ASSERT_NOT_EQUAL(SCENE_ENTITY_NONE, active->subject);
    const r3d_lit_mesh_t* mesh = scene_entity_mesh(active->scene, active->subject);
    TEST_ASSERT_NOT_NULL(mesh);
    TEST_ASSERT_GREATER_THAN_INT(0, mesh->vertex_count);
    heap_report(CAPYBARA_PERF_PHASE_LOADED);
    active->position_bytes = (size_t)mesh->vertex_count * sizeof *mesh->positions;
    active->positions = memory_alloc(active->position_bytes, MEMORY_PSRAM);
    TEST_ASSERT_NOT_NULL(active->positions);
    TEST_ASSERT_NOT_NULL(app_render_lab.console);
    return active;
}

static void
assert_skin_changes(fixture_t* f) {
    const r3d_lit_mesh_t* mesh = scene_entity_mesh(f->scene, f->subject);
    memcpy(f->positions, mesh->positions, f->position_bytes);
    render_lab_test_frame(false);
    TEST_ASSERT_NOT_EQUAL(0, memcmp(f->positions, mesh->positions, f->position_bytes));
}

static void
test_console_gallop_blends_and_skins(void) {
    fixture_t* f = fixture();
    render_lab_test_frame(false);
    char command[ASSET_NAME_MAX];
    (void)snprintf(command, sizeof command, CLIP_COMMAND, CLIP_BLEND_MS);
    TEST_ASSERT_TRUE(app_render_lab.console->handle(command));
    const scene_animator_t* a = &f->scene->animators[f->subject];
    TEST_ASSERT_EQUAL_UINT32(CLIP_BLEND_MS, a->layer.blend_ms);
    TEST_ASSERT_EQUAL_STRING("gallop", scene_animator_clip_name(f->scene, f->subject, a->layer.current.clip));
    render_lab_test_frame(false);
    TEST_ASSERT_EQUAL_UINT32(RENDER_LAB_TEST_FRAME_MS, a->layer.blended_ms);
    TEST_ASSERT_LESS_THAN_UINT32(CLIP_BLEND_MS, a->layer.blended_ms);
    for (int i = 0; i <= CLIP_BLEND_MS / RENDER_LAB_TEST_FRAME_MS; i++) {
        render_lab_test_frame(false);
    }
    TEST_ASSERT_EQUAL_UINT32(CLIP_BLEND_MS, a->layer.blended_ms);
    TEST_ASSERT_EQUAL_STRING("gallop", scene_animator_clip_name(f->scene, f->subject, a->layer.current.clip));
    assert_skin_changes(f);
}

typedef struct {
    scene_t* scene;
    int first;
    int count;
} range_t;

static void
skin_slice(void* ctx) {
    const range_t* r = ctx;
    scene_skin_range(r->scene, r->first, r->count);
}

static void
animate_slice(void* ctx) {
    const range_t* r = ctx;
    scene_animate_range(r->scene, r->first, r->count, RENDER_LAB_TEST_FRAME_MS);
}

static void
step(void* ctx) {
    const range_t* r = ctx;
    scene_animation_step(r->scene, RENDER_LAB_TEST_FRAME_MS);
}

static void
report_stage(fixture_t* f, const char* stage, job_fn_t fn, int count, bool split, const char* cores) {
    const int half = count / 2;
    range_t a = {f->scene, 0, split ? half : count};
    range_t b = {f->scene, half, count - half};
    _Static_assert(sizeof a <= JOB_CTX_MAX, "perf range fits worker context");
    int worker_samples = 0;
    for (int i = -1; i < PERF_SAMPLES; i++) {
        scene_animation_step(f->scene, RENDER_LAB_TEST_FRAME_MS);
        const int64_t start_us = timing_now_us();
        const uint32_t start_cycles = esp_cpu_get_cycle_count();
        bool dispatched = false;
        if (split && half > 0) {
            dispatched = job_run_split(fn, &a, &b, sizeof a);
        } else {
            fn(&a);
        }
        const uint32_t cycles = esp_cpu_get_cycle_count() - start_cycles;
        const int64_t us = timing_now_us() - start_us;
        if (i >= 0) {
            worker_samples += dispatched;
            f->elapsed_us[i] = (int32_t)us;
            f->cpu_cycles[i] = (int32_t)cycles;
        }
    }
    const perf_stats_t us = perf_stats_compute(f->elapsed_us, PERF_SAMPLES);
    const perf_stats_t cycles = perf_stats_compute(f->cpu_cycles, PERF_SAMPLES);
    const int vertices = scene_entity_mesh(f->scene, f->subject)->vertex_count;
    const scene_skinned_renderer_t* skin = &f->scene->skinned_renderers[f->subject];
    TEST_ASSERT_GREATER_THAN_INT64(0, us.avg);
    ESP_LOGI(TAG,
             CAPYBARA_PERF_PREFIX
             " stage=%s cores=%s worker_samples=%d samples=%d vertices=%d" CAPYBARA_PERF_SETUP_FORMAT
             " mean_us=%lld p95_us=%lld "
             "us_per_vertex=%.3f "
             "cycles_per_vertex=%.3f",
             stage, cores, worker_samples, PERF_SAMPLES, vertices, f->scene->animators[f->subject].skeleton.joint_count,
             skin->skin.influences, skin->light_mode, f->scene->light_count, (long long)us.avg, (long long)us.p95,
             (double)us.avg / vertices, (double)cycles.avg / vertices);
}

static perf_stats_t
frame_cost(fixture_t* f, bool paused, perf_stats_t* render_stats) {
    const int joints = f->scene->animators[f->subject].skeleton.joint_count;
    (void)scene_animator_set_paused(f->scene, f->subject, SCENE_ANIMATOR_CURRENT, paused);
    (void)scene_animator_set_paused(f->scene, f->subject, SCENE_ANIMATOR_PREVIOUS, paused);
    for (int i = -1; i < PERF_SAMPLES; i++) {
        const int64_t start = timing_now_us();
        const int64_t render_us = render_lab_test_frame(false);
        if (i >= 0) {
            f->render_us[i] = (int32_t)render_us;
            f->elapsed_us[i] = (int32_t)(timing_now_us() - start);
        }
    }
    *render_stats = perf_stats_compute(f->render_us, PERF_SAMPLES);
    ESP_LOGI(TAG,
             CAPYBARA_PERF_PREFIX " stage=" CAPYBARA_PERF_STAGE_RENDER
                                  " paused=%d samples=%d joints=%d mean_us=%lld p95_us=%lld",
             paused, PERF_SAMPLES, joints, (long long)render_stats->avg, (long long)render_stats->p95);
    const perf_stats_t stats = perf_stats_compute(f->elapsed_us, PERF_SAMPLES);
    TEST_ASSERT_GREATER_THAN_INT64(0, stats.avg);
    ESP_LOGI(TAG,
             CAPYBARA_PERF_PREFIX " stage=" CAPYBARA_PERF_STAGE_FRAME
                                  " paused=%d samples=%d joints=%d mean_us=%lld p95_us=%lld",
             paused, PERF_SAMPLES, joints, (long long)stats.avg, (long long)stats.p95);
    return stats;
}

static void
test_animation_and_frame_cost(void) {
    fixture_t* f = fixture();
    char command[ASSET_NAME_MAX];
    (void)snprintf(command, sizeof command, CLIP_COMMAND, CLIP_CUT_MS);
    TEST_ASSERT_TRUE(app_render_lab.console->handle(command));
    assert_skin_changes(f);
    heap_report(CAPYBARA_PERF_PHASE_PLAYING);
    const int clusters = scene_entity_mesh(f->scene, f->subject)->cluster_count;
    TEST_ASSERT_GREATER_THAN_INT(0, clusters);
    report_stage(f, CAPYBARA_PERF_STAGE_SKIN, skin_slice, clusters, false, "1");
    report_stage(f, CAPYBARA_PERF_STAGE_SKIN, skin_slice, clusters, true, "2");
    report_stage(f, CAPYBARA_PERF_STAGE_ANIMATE, animate_slice, f->scene->asset.entity_count, false, "1");
    report_stage(f, CAPYBARA_PERF_STAGE_ANIMATE, animate_slice, f->scene->asset.entity_count, true, "2");
    report_stage(f, CAPYBARA_PERF_STAGE_ANIMATION_STEP, step, 1, false, "unknown");
    perf_stats_t playing_render, paused_render;
    const perf_stats_t playing = frame_cost(f, false, &playing_render);
    const perf_stats_t paused = frame_cost(f, true, &paused_render);
    const int joints = f->scene->animators[f->subject].skeleton.joint_count;
    ESP_LOGI(TAG, CAPYBARA_PERF_PREFIX " stage=" CAPYBARA_PERF_STAGE_RENDER_DELTA " joints=%d mean_us=%lld p95_us=%lld",
             joints, (long long)(playing_render.avg - paused_render.avg),
             (long long)(playing_render.p95 - paused_render.p95));
    ESP_LOGI(TAG, CAPYBARA_PERF_PREFIX " stage=" CAPYBARA_PERF_STAGE_FRAME_DELTA " joints=%d mean_us=%lld p95_us=%lld",
             joints, (long long)(playing.avg - paused.avg), (long long)(playing.p95 - paused.p95));
}
#endif

void
run_capybara_perf_suite(void) {
#ifdef DEVICE_BUILD
    RUN_TEST(test_console_gallop_blends_and_skins);
    RUN_TEST(test_animation_and_frame_cost);
#endif
}

SUITE_REGISTER(run_capybara_perf_suite);
