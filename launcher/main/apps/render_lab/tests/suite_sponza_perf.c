/*
 * Device-only suite: what a frame of the Sponza showcase bake costs with
 * culling on and off, and of the full bake placed by a moving model, the way
 * scene_sponza.c renders it (half resolution, upscaled), at evenly spaced
 * points of the flythrough. Also each bake's draw stage by stage, and, for
 * the full bake at the flythrough's start, whether its two halves slow each
 * other on both cores and what an empty frame costs.
 * The panel transfer is not included.
 *
 * Runs under DEVICE_BUILD only - needs PSRAM, core 1 and a clock.
 */
#include "suites.h" /* portable - needed by SUITE_REGISTER() even on host */

#ifdef DEVICE_BUILD

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "frame_profile.h"
#include "perf_budget.h"
#include "testkit/bench.h"
#include "testkit/cleanup.h"
#include "unity.h"

#include "esp_log.h"

#include "apps/render_lab/render_lab_view.h"
#include "apps/render_lab/sponza_content.h"
#include "apps/render_lab/tests/one_core_draw.h"
#include "apps/render_lab/tests/r3d_perf_quarters.h"
#include "asset/asset_store.h"
#include "core/job.h"
#include "core/memory.h"
#include "core/timing.h"
#include "core/tune.h"
#include "display/display.h"
#include "display/viewport.h"
#include "gfx/gfx.h"
#include "hash/crc32.h"
#include "math/linear/transformf.h"
#include "profile/frame_cost.h"
#include "render/lod/r3d_lod.h"
#include "render/r3d_lit_mesh.h"
#include "render/r3d_span_internal.h"
#include "render/raster_internal.h"
#include "scene/animator/animator.h"
#include "scene/renderer/renderer.h"
#include "scene/scene.h"
#include "services/build_id.h"
#include "sponza_suite.h"

static const r3d_perf_quarter_t* perf_quarter;

static const char* TAG = "sponza_perf";

/* The shipped meshlet size, set by a capture of other sizes (meshlet_capture.py): a p95 ceiling, pegged on
 * the shipped bakes, then gates only a mesh of that size and logs the rest. 0 gates every mesh. */
TUNE_OWNER(sponza_perf);
TUNE(sponza_perf, shipped_meshlet, 0, 0, UINT16_MAX);

/* The scene, loaded for the suite, its pack, and one mesh for each bake. */
static entity_t sponza;
static const asset_pack_t* pack;
static const lens_t* flythrough;
static entity_t camera_entity;
static r3d_lit_mesh_t meshes[SCENE_BAKED_VARIANT_COUNT];

#define PANEL_PIXELS ((size_t)GFX_WIDTH * GFX_HEIGHT)

typedef struct {
    void* scratch;
    gfx_color_t* panel;
    r3d_instance_t instance; /* the one drawn, with coarse bits when its mesh has levels */
    uint32_t* coarse;
    raster_t raster;
    r3d_lens_t lens;
    char* report;
    int32_t* frame_us;
    raster_buffers_t buffers; /* last: it ends in a target's attachments */
} bench_t;

/* The bench a test has open. The test's cleanup closes it, so a failed
 * assertion cannot leave it holding PSRAM the next test allocates. */
static bench_t* open_bench;

static void
bench_close(void) {
    if (open_bench == NULL) {
        return;
    }
    memory_free(open_bench->frame_us);
    memory_free(open_bench->coarse);
    memory_free(open_bench->report);
    memory_free(open_bench->scratch);
    memory_free(open_bench->panel);
    memory_free(open_bench);
    open_bench = NULL;
}

static bench_t*
bench_open(const r3d_instance_t* instance) {
    bench_t* b = memory_alloc(sizeof(*b), MEMORY_INTERNAL);
    TEST_ASSERT_NOT_NULL(b);
    *b = (bench_t){0};
    open_bench = b;
    suite_add_test_cleanup(bench_close);
    b->panel = memory_alloc(sizeof(gfx_color_t) * PANEL_PIXELS, MEMORY_APP);
    b->instance = *instance;
    if (b->instance.lod_coarse == NULL && instance->mesh->lod_group_count > 0) {
        /* All clear, as a scene's renderer starts: the finest level until a draw picks. */
        const size_t words = R3D_LOD_STATE_WORDS(instance->mesh->lod_group_count);
        b->coarse = memory_alloc(words * sizeof(*b->coarse), MEMORY_INTERNAL);
        TEST_ASSERT_NOT_NULL(b->coarse);
        memset(b->coarse, 0, words * sizeof(*b->coarse));
        b->instance.lod_coarse = b->coarse;
    }
    const resolution_step_t size = render_lab_render_size(GFX_WIDTH, GFX_HEIGHT);
    b->raster = (raster_t){
        .instances = &b->instance,
        .instance_count = 1,
        .width = size.width,
        .height = size.height,
        .upscaled = true,
        .destination_width = GFX_WIDTH,
        .destination_height = GFX_HEIGHT,
    };
    b->raster.present = render_lab_present();
    b->scratch = memory_alloc(raster_scratch_bytes(&b->raster), MEMORY_APP);
    TEST_ASSERT_NOT_NULL(b->scratch);
    TEST_ASSERT_NOT_NULL(b->panel);
    b->raster.scratch = b->scratch;
    b->buffers = raster_carve(&b->raster);
    ESP_LOGI(TAG, "raster=%dx%d present=%d" R3D_PERF_QUARTER_FORMAT, b->raster.width, b->raster.height,
             render_lab_present(), perf_quarter->name);
    ESP_LOGI(TAG, "heap after setup: internal free %u largest %u, psram free %u largest %u" R3D_PERF_QUARTER_FORMAT,
             (unsigned)memory_free_bytes(MEMORY_INTERNAL), (unsigned)memory_largest_block(MEMORY_INTERNAL),
             (unsigned)memory_free_bytes(MEMORY_APP), (unsigned)memory_largest_block(MEMORY_APP), perf_quarter->name);
    return b;
}

static __attribute__((noinline)) void
view_at(const r3d_lit_mesh_t* mesh, uint32_t t_ms, r3d_lens_t* lens) {
    (void)animator_seek(camera_entity, ANIMATOR_CURRENT, t_ms);
    const resolution_step_t size = render_lab_render_size(GFX_WIDTH, GFX_HEIGHT);
    const render_view_t view = sponza_suite_view(flythrough, sponza, camera_entity,
                                                 (viewport_t){size.width, size.height, perf_quarter->quarter});
    r3d_lens_init(lens, &view, mesh->position_scale);
}

/* Out of line: its carved buffers and target stay off the caller's frame. */
static __attribute__((noinline)) void
clear_and_draw(const raster_t* raster, const r3d_lens_t* lens, int visible) {
    const raster_buffers_t b = raster_carve(raster);
    const size_t pixels = (size_t)raster->width * (size_t)raster->height;
    gfx_color_t* color = gfx_target_color(&b.picture, 0);
    for (size_t i = 0; i < pixels; i++) {
        color[i] = raster->clear;
    }
    memset(gfx_target_depth(&b.picture, 0), 0, pixels * sizeof(uint16_t));
    r3d_span_target_t target = {.small_fill = &r3d_span_small_fill_default};
    gfx_target_window(&target.rows, &b.picture, b.picture.first_row, gfx_target_end_row(&b.picture));
    r3d_pipeline_draw(raster->instances[0].mesh, lens, b.culled + 1, visible, b.cs, b.rows, &target, b.work[0]);
}

typedef struct {
    const raster_t* raster;
    const r3d_lens_t* lens;
    int visible, row0, row1;
    int64_t us;
} half_job_t;

static half_job_t core1_result;

static void
draw_half(half_job_t* j) {
    const raster_t* f = j->raster;
    const int64_t start = timing_now_us();
    const raster_buffers_t b = raster_carve(f);
    r3d_span_target_t target = {.small_fill = &r3d_span_small_fill_default};
    gfx_target_window(&target.rows, &b.picture, j->row0, j->row1);
    memset(gfx_target_depth(&target.rows, j->row0), 0,
           (size_t)(j->row1 - j->row0) * (size_t)f->width * sizeof(uint16_t));
    r3d_pipeline_draw(f->instances[0].mesh, j->lens, b.culled + 1, j->visible, b.cs, b.rows, &target,
                      b.work[j->row0 == 0 ? 0 : 1]);
    j->us = timing_now_us() - start;
}

static void
draw_half_on_core1(void* ctx) {
    core1_result = *(const half_job_t*)ctx;
    draw_half(&core1_result);
}

/* Each half of the rows alone, then both at once on the two cores: equal
 * times mean the halves are independent; longer times together mean the
 * cores are waiting on something they share. */
static void
report_core_contention(const raster_t* raster, const r3d_lens_t* lens, int visible) {
    const int mid = raster->height / 2;
    half_job_t top = {raster, lens, visible, 0, mid, 0};
    half_job_t bottom = {raster, lens, visible, mid, raster->height, 0};
    draw_half(&top);
    draw_half(&bottom);
    ESP_LOGI(TAG, "contention: alone  top %6lldus  bottom %6lldus" R3D_PERF_QUARTER_FORMAT, (long long)top.us,
             (long long)bottom.us, perf_quarter->name);
    half_job_t together_top = top;
    half_job_t together_bottom = bottom;
    const int64_t start = timing_now_us();
    if (!job_try_core1(draw_half_on_core1, &together_top, sizeof together_top)) {
        draw_half_on_core1(&together_top);
    }
    draw_half(&together_bottom);
    TEST_ASSERT_TRUE(job_wait(1000));
    const int64_t wall = timing_now_us() - start;
    ESP_LOGI(TAG, "contention: at once top %6lldus  bottom %6lldus  wall %6lldus" R3D_PERF_QUARTER_FORMAT,
             (long long)core1_result.us, (long long)together_bottom.us, (long long)wall, perf_quarter->name);
}

static void
open_the_meshes(void) {
    TEST_ASSERT_FALSE_MESSAGE(handle_is_none(sponza), "scene sponza did not load: see the log above");
    for (int i = 0; i < (int)SCENE_BAKED_VARIANT_COUNT; i++) {
        const entity_t bake = entity_find(sponza, name_hash(sponza_bakes[i]));
        TEST_ASSERT_FALSE_MESSAGE(handle_is_none(bake), sponza_bakes[i]);
        const char* mesh = renderer_mesh_id(bake);
        TEST_ASSERT_NOT_NULL_MESSAGE(mesh, sponza_bakes[i]);
        TEST_ASSERT_EQUAL_INT_MESSAGE(ASSET_OK, r3d_lit_mesh_open(pack, mesh, &meshes[i]), mesh);
    }
}

/* Each bake's draw split into its stages, one core, as the mean over the
 * flythrough's poses: each stage is the difference of two stopped draws. */
static void
report_stage_split(int bake) {
    const r3d_instance_t instance = {&meshes[bake], NULL, NULL, NULL};
    bench_t* b = bench_open(&instance);
    const raster_buffers_t* parts = &b->buffers;
    const r3d_lens_t* lens = &b->lens;
    const uint32_t period = animator_clip_duration_ms(camera_entity, 0);
    int64_t stopped[4] = {0};
    int poses = 0;
    for (uint32_t t_ms = 0; t_ms < period; t_ms += SPONZA_POSE_EVERY_MS) {
        view_at(&meshes[bake], t_ms, &b->lens);
        const int visible = r3d_pipeline_cull(&meshes[bake], lens, NULL, parts->culled + 1, parts->work[0], NULL);
        r3d_pipeline_transform(&meshes[bake], lens, parts->culled + 1, visible, parts->cs, parts->rows, false);
        for (int stop = 0; stop <= 3; stop++) {
            stopped[stop] += one_core_draw_us(&b->raster, parts, lens, visible, stop);
        }
        poses++;
    }
    TEST_ASSERT_GREATER_THAN_INT(0, poses);
    ESP_LOGI(TAG,
             "stage_split: bake=%s %dx%d poses=%d tris=%d one core us/pose setup %lld edges %lld carry %lld fill "
             "%lld" R3D_PERF_QUARTER_FORMAT,
             sponza_bakes[bake], b->raster.width, b->raster.height, poses, meshes[bake].triangle_count,
             (long long)(stopped[1] / poses), (long long)((stopped[2] - stopped[1]) / poses),
             (long long)((stopped[3] - stopped[2]) / poses), (long long)((stopped[0] - stopped[3]) / poses),
             perf_quarter->name);
    bench_close();
}

void
test_sponza_draw_stage_breakdown(void) {
    open_the_meshes();
    for (int i = 0; i < (int)SCENE_BAKED_VARIANT_COUNT; i++) {
        report_stage_split(i);
    }
    const r3d_instance_t atrium = {&meshes[SCENE_BAKED_FULL], NULL, NULL, NULL};
    bench_t* b = bench_open(&atrium);
    view_at(atrium.mesh, 0, &b->lens);
    const r3d_lens_t* lens = &b->lens;
    const raster_buffers_t* parts = &b->buffers;
    const int visible = r3d_pipeline_cull(atrium.mesh, lens, NULL, parts->culled + 1, parts->work[0], NULL);
    int64_t start = timing_now_us();
    r3d_pipeline_transform(atrium.mesh, lens, parts->culled + 1, visible, parts->cs, parts->rows, false);
    ESP_LOGI(TAG, "stage, one core: %-22s %7lldus" R3D_PERF_QUARTER_FORMAT, "transform",
             (long long)(timing_now_us() - start), perf_quarter->name);

    static const char* const names[4] = {"whole draw", "stop after setup", "stop after edges",
                                         "stop after attribute carry"};
    for (int stop = 0; stop <= 3; stop++) {
        r3d_span_stop_after = stop;
        start = timing_now_us();
        clear_and_draw(&b->raster, lens, visible);
        ESP_LOGI(TAG, "stage, one core: %-22s %7lldus" R3D_PERF_QUARTER_FORMAT, names[stop],
                 (long long)(timing_now_us() - start), perf_quarter->name);
    }
    r3d_span_stop_after = 0;
    report_core_contention(&b->raster, lens, visible);

    /* A view of nothing but sky: what a frame costs before any geometry. */
    const transformf_t sky_pose =
        transformf_looking((vec3f_t){0.0F, 20000.0F, 0.0F}, (vec3f_t){0.0F, 1.0F, 0.01F}, vec3f_unit_y());
    start = timing_now_us();
    const render_view_t frame_view =
        render_view_make(&sky_pose, 1.0F, 1.0F, (viewport_t){GFX_WIDTH, GFX_HEIGHT, perf_quarter->quarter});
    const raster_stats_t none = raster_draw(&b->raster, &frame_view);
    const resolution_step_t composed = raster_compose_size(&b->raster);
    raster_upscale(&b->raster, b->panel, composed.width, composed.height);
    ESP_LOGI(TAG, "stage, both cores: %-20s %7lldus (%d clusters)" R3D_PERF_QUARTER_FORMAT, "empty frame",
             (long long)(timing_now_us() - start), none.clusters, perf_quarter->name);

    bench_close();
    TEST_PASS();
}

/* `moving`, when not NULL, is the pose behind the instance's `model`,
 * nudged every frame. */
static void
report_frame_cost(const char* label, const char* object_name, const r3d_instance_t* instance, transformf_t* moving,
                  mat4f_t* model, const perf_budget_t budget[R3D_PERF_QUARTER_COUNT]) {
    const r3d_lit_mesh_t* mesh = instance->mesh;
    bench_t* b = bench_open(instance);
    ESP_LOGI(TAG, "=== %s FRAME COST (%d tris, %d verts, %d clusters, rendered %dx%d) ===" R3D_PERF_QUARTER_FORMAT,
             label, mesh->triangle_count, mesh->vertex_count, mesh->cluster_count, b->raster.width, b->raster.height,
             perf_quarter->name);
    int size = 0;
    for (int i = 0; i < mesh->cluster_count; i++) {
        if (mesh->clusters[i].triangle_count > size) {
            size = mesh->clusters[i].triangle_count;
        }
    }
    const uint32_t period = animator_clip_duration_ms(camera_entity, 0);
    ESP_LOGI(TAG,
             "CAPTURE build_id=%s pack_crc32=%08x object=%s size=%d cull=%ld period_ms=%u "
             "pose_every_ms=%u raster=%dx%d present=%d" R3D_PERF_QUARTER_FORMAT,
             build_id(), (unsigned)crc32_update(0, pack->base, pack->size), object_name, size,
             (long)raster_settings(&b->raster).cull, (unsigned)period, (unsigned)SPONZA_PERF_POSE_EVERY_MS,
             b->raster.width, b->raster.height, render_lab_present(), perf_quarter->name);
    b->report = memory_alloc(FRAME_COST_REPORT_MAX, MEMORY_INTERNAL);
    char* report = b->report;
    TEST_ASSERT_NOT_NULL(report);
    (void)frame_cost_take_report(1, report, FRAME_COST_REPORT_MAX);
    const int capacity = (int)((period + SPONZA_PERF_POSE_EVERY_MS - 1) / SPONZA_PERF_POSE_EVERY_MS);
    TEST_ASSERT_GREATER_THAN_INT(0, capacity);
    b->frame_us = memory_alloc((size_t)capacity * sizeof *b->frame_us, MEMORY_APP);
    int32_t* frame_us = b->frame_us;
    TEST_ASSERT_NOT_NULL(frame_us);
    int64_t frame_sum = 0;
    int samples = 0;
    const int pick_every = raster_settings(&b->raster).lod.pick_every;
    for (uint32_t t_ms = 0; t_ms < period; t_ms += SPONZA_PERF_POSE_EVERY_MS) {
        (void)animator_seek(camera_entity, ANIMATOR_CURRENT, t_ms);
        const render_view_t view = sponza_suite_view(flythrough, sponza, camera_entity,
                                                     (viewport_t){GFX_WIDTH, GFX_HEIGHT, perf_quarter->quarter});
        if (moving != NULL) {
            transformf_set_position(moving,
                                    (vec3f_t){(t_ms / SPONZA_PERF_POSE_EVERY_MS) % 2 == 0 ? 0.0F : 0.01F, 0.0F, 0.0F});
            *model = transformf_compute_matrix(moving);
        }
        /* The levels the frame draws, picked as render_context_draw() picks them, inside the frame's time:
         * a pick spread over frames has the pose's earlier frames drawn first, untimed. */
        b->raster.picks_levels = true;
        for (int warm = 1; warm < pick_every; warm++) {
            b->raster.pick_round++;
            (void)raster_draw(&b->raster, &view);
        }
        b->raster.pick_round++;
        const int64_t start = timing_now_us();
        const raster_stats_t stats = raster_draw(&b->raster, &view);
        b->raster.picks_levels = false;
        const resolution_step_t composed = raster_compose_size(&b->raster);
        raster_upscale(&b->raster, b->panel, composed.width, composed.height);
        const int64_t us = timing_now_us() - start;
        ESP_LOGI(TAG, FRAME_PROFILE_POSE R3D_PERF_QUARTER_FORMAT, label, (unsigned)t_ms, stats.clusters,
                 stats.triangles, (long long)us, perf_quarter->name);
        (void)frame_cost_take_report(1, report, FRAME_COST_REPORT_MAX);
        ESP_LOGI(TAG, "ms/frame avg/worst: %s" R3D_PERF_QUARTER_FORMAT, report, perf_quarter->name);
        frame_sum += us;
        TEST_ASSERT_LESS_OR_EQUAL_INT64(INT32_MAX, us);
        frame_us[samples] = (int32_t)us;
        samples++;
    }
    const bench_stats_t stats = bench_stats_compute(frame_us, samples);
    ESP_LOGI(TAG, FRAME_PROFILE_ROW R3D_PERF_QUARTER_FORMAT, label, (long long)stats.avg,
             1e6 * samples / (double)frame_sum, (long long)stats.max, (long long)stats.med, (long long)stats.p95,
             perf_quarter->name);
    const size_t q = (size_t)(perf_quarter - r3d_perf_quarters);
    const bool ceiling_applies = shipped_meshlet == 0 || size == shipped_meshlet;
    r3d_perf_budget(label, perf_quarter, stats.avg, stats.p95, &budget[q], ceiling_applies);
    if (!ceiling_applies) {
        ESP_LOGI(TAG, "%s: p95 %lldus, ceiling %lldus gates only meshlets of %ld" R3D_PERF_QUARTER_FORMAT, label,
                 (long long)stats.p95, (long long)budget[q].p95_ceiling_us, (long)shipped_meshlet, perf_quarter->name);
    }
    bench_close();
}

/* The scene's showcase: the bake of its first renderer. */
static scene_baked_variant_t
showcase_bake(void) {
    for (entity_t entity = entity_next(sponza, ENTITY_NONE); !handle_is_none(entity);
         entity = entity_next(sponza, entity)) {
        if (renderer_mesh_id(entity) == NULL) {
            continue;
        }
        for (int bake = 0; bake < (int)SCENE_BAKED_VARIANT_COUNT; bake++) {
            if (strcmp(entity_name(entity), sponza_bakes[bake]) == 0) {
                return (scene_baked_variant_t)bake;
            }
        }
        TEST_FAIL_MESSAGE(entity_name(entity));
    }
    TEST_FAIL_MESSAGE("the scene has no renderer");
    return SCENE_BAKED_VARIANT_COUNT;
}

static int32_t saved_cull;

static void
restore_culling(void) {
    (void)tune_set(tune_shared(), TUNE_NAME(render, cull), saved_cull);
}

/* The meshes open and culling at its default, restored after the test. */
static void
open_with_default_culling(void) {
    /* Cache configuration trades internal RAM for speed, so a frame-cost
     * capture carries what it left. */
    ESP_LOGI(TAG, "internal heap: free %u largest %u" R3D_PERF_QUARTER_FORMAT,
             (unsigned)memory_free_bytes(MEMORY_INTERNAL), (unsigned)memory_largest_block(MEMORY_INTERNAL),
             perf_quarter->name);
    open_the_meshes();
    const tune_entry_t* culling = tune_find(tune_shared(), TUNE_NAME(render, cull));
    TEST_ASSERT_NOT_NULL(culling);
    saved_cull = *culling->value;
    suite_add_test_cleanup(restore_culling);
    TEST_ASSERT_TRUE(tune_reset(tune_shared(), TUNE_NAME(render, cull)));
}

void
test_sponza_frame_cost_along_the_flythrough(void) {
    open_with_default_culling();
    /* Worst of a 5-seed capture on A308 (before and after its move), 20E8 and
     * 2124, + 1%; per quarter, portrait then landscape. The showcase's pair is
     * what a meshlet size capture reads; run_sponza_bakes_suite times every bake. */
    static const perf_budget_t showcase_budget[R3D_PERF_QUARTER_COUNT] = {{41877, 53439}, {39437, 49049}};
    static const perf_budget_t cull_off_budget[R3D_PERF_QUARTER_COUNT] = {{61138, 67896}, {58104, 66246}};
    const scene_baked_variant_t showcase = showcase_bake();
    TEST_ASSERT_EQUAL_INT_MESSAGE(SCENE_BAKED_FITTED_FULL, showcase, "the showcase budgets are fitted full's");
    const r3d_instance_t shown = {&meshes[showcase], NULL, NULL, NULL};
    report_frame_cost(sponza_bakes[showcase], sponza_bakes[showcase], &shown, NULL, NULL, showcase_budget);
    TEST_ASSERT_TRUE(tune_set(tune_shared(), TUNE_NAME(render, cull), 0));
    report_frame_cost("cull_off", sponza_bakes[showcase], &shown, NULL, NULL, cull_off_budget);
    TEST_PASS();
}

/* The full bake placed by a model matrix nudged every frame. */
void
test_sponza_frame_cost_placed(void) {
    open_the_meshes();
    /* Worst of a 5-seed capture on A308 (before and after its move), 20E8 and
     * 2124, + 1%; per quarter, portrait then landscape. */
    static const perf_budget_t placed_budget[R3D_PERF_QUARTER_COUNT] = {{41115, 52181}, {38830, 47638}};
    transformf_t pose = transformf_identity();
    mat4f_t model = transformf_compute_matrix(&pose);
    const r3d_instance_t moving = {&meshes[0], &model, NULL, NULL};
    report_frame_cost("placed", sponza_bakes[0], &moving, &pose, &model, placed_budget);
    TEST_PASS();
}

/* Every bake along the flythrough, unpegged: the docs' board stage
 * (render_doc_images.sh --stage board) reads these rows. */
void
test_sponza_bake_frame_costs(void) {
    open_with_default_culling();
    static const perf_budget_t unpegged[R3D_PERF_QUARTER_COUNT] = {{0}};
    for (int i = 0; i < (int)SCENE_BAKED_VARIANT_COUNT; i++) {
        const r3d_instance_t instance = {&meshes[i], NULL, NULL, NULL};
        report_frame_cost(sponza_bakes[i], sponza_bakes[i], &instance, NULL, NULL, unpegged);
    }
    TEST_PASS();
}

/* `tests` once per perf quarter, with the scene loaded. */
static void
run_with_sponza(void (*tests)(void)) {
    sponza_suite_t loaded = sponza_suite_load();
    sponza = loaded.scene;
    pack = asset_store_loaded(loaded.load);
    flythrough = loaded.lens;
    camera_entity = handle_is_none(sponza) ? ENTITY_NONE : entity_find(sponza, name_hash(sponza_cameras[0]));
    (void)render_lab_apply_camera_settings(camera_entity);
    for (size_t q = 0; q < R3D_PERF_QUARTER_COUNT; q++) {
        perf_quarter = &r3d_perf_quarters[q];
        tests();
    }
    loaded = (sponza_suite_t){sponza, loaded.load, flythrough, loaded.arena_mark};
    sponza_suite_release(&loaded);
    sponza = ENTITY_NONE;
    pack = NULL;
    flythrough = NULL;
}

static void
perf_tests(void) {
    RUN_TEST_CASE(test_sponza_draw_stage_breakdown, perf_quarter->name);
    RUN_TEST_CASE(test_sponza_frame_cost_along_the_flythrough, perf_quarter->name);
    RUN_TEST_CASE(test_sponza_frame_cost_placed, perf_quarter->name);
}

static void
bake_tests(void) {
    RUN_TEST_CASE(test_sponza_bake_frame_costs, perf_quarter->name);
}

void
run_sponza_perf_suite(void) {
    run_with_sponza(perf_tests);
}

void
run_sponza_bakes_suite(void) {
    run_with_sponza(bake_tests);
}

#else /* !DEVICE_BUILD */

void
run_sponza_perf_suite(void) {}

void
run_sponza_bakes_suite(void) {}

#endif /* DEVICE_BUILD */

SUITE_REGISTER(run_sponza_perf_suite);
/* On request: the docs' board stage, not a perf check. */
SUITE_REGISTER_ON_REQUEST(run_sponza_bakes_suite);
