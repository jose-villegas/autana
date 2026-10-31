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

#include "app/app_arena.h"
#include "apps/render_lab/baked_families.h"
#include "apps/render_lab/render_lab_view.h"
#include "apps/render_lab/tests/one_core_draw.h"
#include "apps/render_lab/tests/r3d_perf_quarters.h"
#include "asset/asset_store.h"
#include "core/memory.h"
#include "core/timing.h"
#include "gfx/gfx.h"
#include "render/r3d_span_internal.h"
#include "render/raster_internal.h"
#include "render/view/r3d_pipeline_count.h"
#include "scene/scene.h"

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

/* One core: cull, transform, the draw stopped after set-up, the whole draw;
 * each the best of CULL_COST_RUNS. */
static stage_us_t
time_stages(const raster_t* raster, const r3d_lit_mesh_t* mesh, const r3d_lens_t* lens, int* visible) {
    const raster_buffers_t b = raster_carve(raster);
    raster_t drawn = *raster;
    const r3d_instance_t instance = {mesh, NULL};
    drawn.instances = &instance;
    stage_us_t us = {INT64_MAX, INT64_MAX, INT64_MAX, INT64_MAX};
    for (int run = 0; run < CULL_COST_RUNS; run++) {
        int64_t start = timing_now_us();
        *visible = r3d_pipeline_cull(mesh, lens, b.culled + 1, b.work[0], NULL);
        us.cull = best(us.cull, timing_now_us() - start);
        start = timing_now_us();
        r3d_pipeline_transform(mesh, lens, b.culled + 1, *visible, b.cs, b.rows);
        us.transform = best(us.transform, timing_now_us() - start);
        us.setup = best(us.setup, one_core_draw_us(&drawn, &b, lens, *visible, R3D_SPAN_STOP_AFTER_SETUP));
        us.draw = best(us.draw, one_core_draw_us(&drawn, &b, lens, *visible, R3D_SPAN_STOP_NONE));
    }
    return us;
}

static void
report_pose(const char* scene, const char* bake, uint32_t t_ms, variant_t variant, const raster_t* raster,
            const r3d_lit_mesh_t* mesh, const r3d_lens_t* lens) {
    int visible = 0;
    const stage_us_t us = time_stages(raster, mesh, lens, &visible);
    const raster_buffers_t b = raster_carve(raster);
    r3d_pipeline_counts_t n = {0};
    r3d_pipeline_count_walk_t* walk = memory_alloc(sizeof(*walk), MEMORY_PSRAM);
    TEST_ASSERT_NOT_NULL(walk);
    TEST_ASSERT_EQUAL_INT(visible, r3d_pipeline_cull_counted(mesh, lens, b.culled + 1, walk, &n));
    memory_free(walk);
    r3d_pipeline_transform(mesh, lens, b.culled + 1, visible, b.cs, b.rows);
    r3d_pipeline_count_triangles(mesh, lens, b.culled + 1, visible, b.cs, &n, NULL);
    int vertices = 0;
    for (int i = 0; i < visible; i++) {
        vertices += mesh->clusters[b.culled[1 + i]].vertex_count;
    }
    ESP_LOGI(TAG,
             "cull_cost: scene=%s bake=%s t_ms=%u variant=%s %dx%d nodes=%d cut=%d tested=%d clusters=%d vertices=%d "
             "submitted=%d behind=%d clipped=%d missed=%d facing_away=%d set_up=%d cull_us=%lld transform_us=%lld "
             "setup_us=%lld draw_us=%lld" R3D_PERF_QUARTER_FORMAT,
             scene, bake, (unsigned)t_ms, variant_names[variant], raster->width, raster->height, n.nodes_visited,
             n.nodes_cut, n.clusters_tested, n.clusters_kept, vertices, n.triangles_submitted, n.triangles_behind,
             n.triangles_clipped, n.triangles_missed, n.triangles_facing_away, n.triangles_set_up, (long long)us.cull,
             (long long)us.transform, (long long)us.setup, (long long)us.draw, r3d_perf_quarters[0].name);
}

static void
measure_family(const scene_baked_t* family) {
    const size_t mark = app_arena_mark();
    scene_failure_t why;
    scene_t* scene = scene_load(family->scene, &why);
    TEST_ASSERT_NOT_NULL_MESSAGE(scene, why.what);
    const handle_t load = asset_store_load(family->scene);
    const asset_pack_t* pack = asset_store_loaded(load);
    const char* bake = family->bakes[SCENE_BAKED_FITTED_FULL];
    const scene_entity_t entity = scene_find(scene, name_hash(bake));
    TEST_ASSERT_NOT_EQUAL_MESSAGE(SCENE_ENTITY_NONE, entity, bake);
    r3d_lit_mesh_t mesh;
    TEST_ASSERT_EQUAL_INT(ASSET_OK, r3d_lit_mesh_open(pack, scene_entity_mesh_id(scene, entity), &mesh));
    const scene_entity_t camera = scene_find(scene, name_hash(family->camera));
    TEST_ASSERT_NOT_EQUAL(SCENE_ENTITY_NONE, camera);
    const camera_t* lens_of = scene_camera_lens(scene, name_hash(family->camera));
    (void)render_lab_apply_camera_settings(scene, camera);

    /* The halved bake: the same clusters, each keeping its first half of triangles. */
    const size_t cluster_bytes = sizeof(r3d_lit_cluster_t) * (size_t)mesh.cluster_count;
    r3d_lit_cluster_t* halved_clusters = memory_alloc(cluster_bytes, MEMORY_PSRAM);
    TEST_ASSERT_NOT_NULL(halved_clusters);
    memcpy(halved_clusters, mesh.clusters, cluster_bytes);
    for (int c = 0; c < mesh.cluster_count; c++) {
        halved_clusters[c].triangle_count = (uint16_t)((halved_clusters[c].triangle_count + 1) / 2);
    }
    r3d_lit_mesh_t halved = mesh;
    halved.clusters = halved_clusters;

    const r3d_instance_t instance = {&mesh, NULL};
    const resolution_step_t size = render_lab_render_size(GFX_WIDTH, GFX_HEIGHT);
    raster_t raster = {.instances = &instance,
                       .instance_count = 1,
                       .width = size.width,
                       .height = size.height,
                       .upscaled = true,
                       .destination_width = GFX_WIDTH,
                       .destination_height = GFX_HEIGHT};
    raster.scratch = memory_alloc(raster_scratch_bytes(&raster), MEMORY_PSRAM);
    TEST_ASSERT_NOT_NULL(raster.scratch);

    const uint32_t period = scene_animator_clip_duration_ms(scene, camera, 0);
    for (uint32_t t_ms = 0; t_ms < period; t_ms += CULL_COST_POSE_EVERY_MS) {
        (void)scene_animator_seek(scene, camera, SCENE_ANIMATOR_CURRENT, t_ms);
        const transformf_t pose = scene_entity_world(scene, camera);
        const render_view_t view =
            camera_view(lens_of, &pose, (viewport_t){size.width, size.height, r3d_perf_quarters[0].quarter});
        r3d_lens_t lens;
        r3d_lens_init(&lens, &view, mesh.position_scale);
        r3d_lens_t mirrored = lens;
        for (int col = 0; col < 4; col++) {
            mirrored.m.m[0][col] = -mirrored.m.m[0][col];
        }
        report_pose(family->scene, bake, t_ms, VARIANT_BAKED, &raster, &mesh, &lens);
        report_pose(family->scene, bake, t_ms, VARIANT_HALVED, &raster, &halved, &lens);
        report_pose(family->scene, bake, t_ms, VARIANT_MIRRORED, &raster, &mesh, &mirrored);
    }

    memory_free(raster.scratch);
    memory_free(halved_clusters);
    asset_store_release(load);
    scene_unload(scene);
    app_arena_rewind(mark);
}

static void
test_cull_cost_along_each_path(void) {
    for (int f = 0; f < BAKED_FAMILY_COUNT; f++) {
        measure_family(baked_families[f]);
    }
    TEST_PASS();
}

void
run_cull_cost_suite(void) {
    RUN_TEST(test_cull_cost_along_each_path);
}

#else /* !DEVICE_BUILD */

void
run_cull_cost_suite(void) {}

#endif /* DEVICE_BUILD */

SUITE_REGISTER(run_cull_cost_suite);
