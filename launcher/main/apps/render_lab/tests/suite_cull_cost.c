/*
 * Device-only suite: what a kept cluster and a submitted triangle each cost
 * one core, apart. Along each family's camera path, the fitted full bake is
 * culled, transformed and drawn three ways that move the counts apart: as
 * baked; with each cluster's triangles halved (the same clusters and
 * vertices, half the triangles submitted); and through a mirrored lens (the
 * same clusters and triangles, the other faces set up). Each line carries
 * the pose's counts beside its times, for cull_cost.py to fit.
 *
 * Runs under DEVICE_BUILD only - needs PSRAM and a clock.
 */
#include "suites.h" /* portable - needed by SUITE_REGISTER() even on host */

#ifdef DEVICE_BUILD

#include <stdint.h>
#include <string.h>

#include "unity.h"

#include "esp_log.h"

#include "apps/render_lab/baked_families.h"
#include "apps/render_lab/render_lab_view.h"
#include "apps/render_lab/tests/one_core_draw.h"
#include "apps/render_lab/tests/scene_suite.h"
#include "asset/asset_store.h"
#include "core/memory.h"
#include "core/timing.h"
#include "gfx/gfx.h"
#include "lab/test/lab_perf_quarters.h"
#include "math/scalar/units.h"
#include "render/r3d_span_internal.h"
#include "render/raster_internal.h"
#include "render/view/r3d_pipeline_count.h"
#include "scene/animator/animator.h"
#include "scene/camera/camera.h"
#include "scene/renderer/renderer.h"
#include "scene/scene.h"
#include "scene/transform/transform.h"

static const char* TAG = "cull_cost";

/* Poses along each path, and the best of this many runs of each stage. */
#define CULL_COST_POSE_EVERY_MS 2500
#define CULL_COST_RUNS          3

typedef enum { VARIANT_BAKED, VARIANT_HALVED, VARIANT_MIRRORED, VARIANT_COUNT } variant_t;

static const char* const variant_names[VARIANT_COUNT] = {"baked", "halved", "mirrored"};

typedef struct {
    int64_t cull, transform, setup, draw;
} stage_us_t;

static int64_t
best(int64_t a, int64_t b) {
    return a < b ? a : b;
}

typedef struct {
    raster_t drawn;
    r3d_instance_t instance;
    stage_us_t us;
    r3d_pipeline_counts_t counts;
    r3d_pipeline_count_walk_t walk;
    int visible;
    raster_buffers_t buffers;
} cull_sample_t;

static __attribute__((noinline)) void
prepare_sample(cull_sample_t* sample, const raster_t* raster, const r3d_lit_mesh_t* mesh) {
    sample->buffers = raster_carve(raster);
    sample->drawn = *raster;
    sample->instance = (r3d_instance_t){mesh, NULL, NULL, NULL};
    sample->drawn.instances = &sample->instance;
    sample->us = (stage_us_t){INT64_MAX, INT64_MAX, INT64_MAX, INT64_MAX};
    sample->counts = (r3d_pipeline_counts_t){0};
}

/* One core: cull, transform, the draw stopped after set-up, the whole draw;
 * each the best of CULL_COST_RUNS. */
static __attribute__((noinline)) void
time_stages(cull_sample_t* sample, const r3d_lit_mesh_t* mesh, const r3d_lens_t* lens) {
    const raster_buffers_t* b = &sample->buffers;
    for (int run = 0; run < CULL_COST_RUNS; run++) {
        int64_t start = timing_now_us();
        sample->visible = r3d_pipeline_cull(mesh, lens, NULL, b->culled + 1, b->work[0], NULL);
        sample->us.cull = best(sample->us.cull, timing_now_us() - start);
        start = timing_now_us();
        r3d_pipeline_transform(mesh, lens, b->culled + 1, sample->visible, b->cs, b->rows, false);
        sample->us.transform = best(sample->us.transform, timing_now_us() - start);
        sample->us.setup = best(sample->us.setup,
                                one_core_draw_us(&sample->drawn, b, lens, sample->visible, R3D_SPAN_STOP_AFTER_SETUP));
        sample->us.draw =
            best(sample->us.draw, one_core_draw_us(&sample->drawn, b, lens, sample->visible, R3D_SPAN_STOP_NONE));
    }
}

static void
report_pose(const char* scene, const char* bake, uint32_t t_ms, variant_t variant, const raster_t* raster,
            const r3d_lit_mesh_t* mesh, const r3d_lens_t* lens) {
    cull_sample_t* const sample = memory_alloc(sizeof *sample, MEMORY_APP);
    TEST_ASSERT_NOT_NULL(sample);
    prepare_sample(sample, raster, mesh);
    time_stages(sample, mesh, lens);
    const int visible = sample->visible;
    const raster_buffers_t* b = &sample->buffers;
    TEST_ASSERT_EQUAL_INT(
        visible, r3d_pipeline_cull_counted(mesh, lens, NULL, NULL, b->culled + 1, &sample->walk, &sample->counts));
    r3d_pipeline_transform(mesh, lens, b->culled + 1, visible, b->cs, b->rows, false);
    r3d_pipeline_count_triangles(mesh, lens, b->culled + 1, visible, b->cs, &sample->counts, NULL);
    int vertices = 0;
    for (int i = 0; i < visible; i++) {
        vertices += mesh->clusters[b->culled[1 + i]].vertex_count;
    }
    ESP_LOGI(TAG,
             "cull_cost: scene=%s bake=%s t_ms=%u variant=%s %dx%d nodes=%d cut=%d tested=%d clusters=%d vertices=%d "
             "submitted=%d behind=%d clipped=%d missed=%d facing_away=%d set_up=%d cull_us=%lld transform_us=%lld "
             "setup_us=%lld draw_us=%lld" LAB_PERF_QUARTER_FORMAT,
             scene, bake, (unsigned)t_ms, variant_names[variant], raster->width, raster->height,
             sample->counts.nodes_visited, sample->counts.nodes_cut, sample->counts.clusters_tested,
             sample->counts.clusters_kept, vertices, sample->counts.triangles_submitted,
             sample->counts.triangles_behind, sample->counts.triangles_clipped, sample->counts.triangles_missed,
             sample->counts.triangles_facing_away, sample->counts.triangles_set_up, (long long)sample->us.cull,
             (long long)sample->us.transform, (long long)sample->us.setup, (long long)sample->us.draw,
             lab_perf_quarters[0].name);
    memory_free(sample);
}

static __attribute__((noinline)) void
cull_pose_lens(r3d_lens_t* out, entity_t camera, const lens_t* lens, resolution_step_t size, int position_scale) {
    const transformf_t pose = transform_world(camera);
    const render_view_t view =
        lens_view(lens, &pose, (viewport_t){size.width, size.height, lab_perf_quarters[0].quarter});
    r3d_lens_init(out, &view, position_scale);
}

static void
measure_family(const scene_baked_t* family) {
    struct {
        r3d_lit_mesh_t mesh, halved;
        raster_t raster;
        r3d_lens_t lens, mirrored;
    }* const work = memory_alloc(sizeof *work, MEMORY_APP);

    TEST_ASSERT_NOT_NULL(work);
    scene_suite_t loaded = scene_suite_load(family->scene);
    const entity_t scene = loaded.scene;
    TEST_ASSERT_FALSE_MESSAGE(handle_is_none(scene), family->scene);
    const handle_t load = asset_store_load(family->scene);
    const asset_pack_t* pack = asset_store_loaded(load);
    const char* bake = family->bake;
    const entity_t entity = entity_find(scene, name_hash(bake));
    TEST_ASSERT_FALSE_MESSAGE(handle_is_none(entity), bake);
    TEST_ASSERT_EQUAL_INT(ASSET_OK, r3d_lit_mesh_open(pack, renderer_mesh_id(entity), &work->mesh));
    const entity_t camera = entity_find(scene, name_hash(family->camera));
    TEST_ASSERT_FALSE(handle_is_none(camera));
    const lens_t* lens_of = camera_lens(entity_find(scene, name_hash(family->camera)));
    (void)render_lab_apply_camera_settings(camera);

    /* The halved bake: the same clusters, each keeping its first half of triangles. */
    const size_t cluster_bytes = sizeof(r3d_lit_cluster_t) * (size_t)work->mesh.cluster_count;
    r3d_lit_cluster_t* halved_clusters = memory_alloc(cluster_bytes, MEMORY_APP);
    TEST_ASSERT_NOT_NULL(halved_clusters);
    memcpy(halved_clusters, work->mesh.clusters, cluster_bytes);
    for (int c = 0; c < work->mesh.cluster_count; c++) {
        halved_clusters[c].triangle_count = (uint16_t)((halved_clusters[c].triangle_count + 1) / 2);
    }
    work->halved = work->mesh;
    work->halved.clusters = halved_clusters;

    const r3d_instance_t instance = {&work->mesh, NULL, NULL, NULL};
    const resolution_step_t size = render_lab_render_size(GFX_WIDTH, GFX_HEIGHT);
    work->raster = (raster_t){.instances = &instance,
                              .instance_count = 1,
                              .width = size.width,
                              .height = size.height,
                              .upscaled = true,
                              .destination_width = GFX_WIDTH,
                              .destination_height = GFX_HEIGHT};
    work->raster.scratch = memory_alloc(raster_scratch_bytes(&work->raster), MEMORY_APP);
    TEST_ASSERT_NOT_NULL(work->raster.scratch);

    const uint32_t period = animator_clip_duration_ms(camera, 0);
    for (uint32_t t_ms = 0; t_ms < period; t_ms += CULL_COST_POSE_EVERY_MS) {
        (void)animator_seek(camera, ANIMATOR_CURRENT, t_ms);
        cull_pose_lens(&work->lens, camera, lens_of, size, work->mesh.position_scale);
        work->mirrored = work->lens;
        for (int col = 0; col < 4; col++) {
            work->mirrored.m.m[0][col] = -work->mirrored.m.m[0][col];
        }
        report_pose(family->scene, bake, t_ms, VARIANT_BAKED, &work->raster, &work->mesh, &work->lens);
        report_pose(family->scene, bake, t_ms, VARIANT_HALVED, &work->raster, &work->halved, &work->lens);
        report_pose(family->scene, bake, t_ms, VARIANT_MIRRORED, &work->raster, &work->mesh, &work->mirrored);
    }

    memory_free(work->raster.scratch);
    memory_free(halved_clusters);
    asset_store_release(load);
    scene_suite_release(&loaded);
    memory_free(work);
}

static void
test_cull_cost_along_each_path(void) {
    for (int f = 0; f < BAKED_FAMILY_COUNT; f++) {
        measure_family(baked_families[f]);
    }
    TEST_PASS();
}

#define HIDDEN_SCAN_WIDTH  64
#define HIDDEN_SCAN_HEIGHT 48
#define HIDDEN_SCAN_PIXELS (HIDDEN_SCAN_WIDTH * HIDDEN_SCAN_HEIGHT)
#define HIDDEN_SCAN_RUNS   256

static void
test_hidden_cluster_scan_cost(void) {
    uint16_t* const pixels = memory_alloc(2 * HIDDEN_SCAN_PIXELS * sizeof *pixels, MEMORY_APP);
    TEST_ASSERT_NOT_NULL(pixels);
    uint16_t* const depth = pixels + HIDDEN_SCAN_PIXELS;
    for (int i = 0; i < HIDDEN_SCAN_PIXELS; i++) {
        depth[i] = R3D_DEPTH_NEAREST;
    }
    r3d_span_target_t target = r3d_span_target(pixels, depth, HIDDEN_SCAN_WIDTH, 0, HIDDEN_SCAN_HEIGHT);
    r3d_hidden_slice_t slice = {0};
    const r3d_pipeline_rows_t rows = {.x1 = HIDDEN_SCAN_WIDTH * R3D_SUBPIXEL,
                                      .y1 = HIDDEN_SCAN_HEIGHT * R3D_SUBPIXEL,
                                      .depth = R3D_DEPTH_NEAREST - 1};
    const int64_t start = timing_now_us();
    for (int i = 0; i < HIDDEN_SCAN_RUNS; i++) {
        (void)r3d_pipeline_cluster_hidden(&rows, &target, &slice);
    }
    const int64_t elapsed = timing_now_us() - start;
    memory_free(pixels);

    const int64_t scanned = HIDDEN_SCAN_RUNS * HIDDEN_SCAN_PIXELS;
    TEST_ASSERT_EQUAL_UINT32(HIDDEN_SCAN_RUNS, slice.tally.skipped);
    TEST_ASSERT_EQUAL_UINT64(scanned, slice.tally.pixels_read);
    ESP_LOGI(TAG, "hidden cluster scan: %lld ns/px (%lld us over %lld pixels)",
             (long long)((elapsed * MATH_NS_PER_US + scanned / 2) / scanned), (long long)elapsed, (long long)scanned);
}

void
run_cull_cost_suite(void) {
    RUN_TEST(test_hidden_cluster_scan_cost);
    RUN_TEST(test_cull_cost_along_each_path);
}

#else /* !DEVICE_BUILD */

void
run_cull_cost_suite(void) {}

#endif /* DEVICE_BUILD */

SUITE_REGISTER(run_cull_cost_suite);
