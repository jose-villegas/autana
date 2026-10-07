/*
 * Device-only suite: what a frame of each Sponza bake costs, the way
 * scene_sponza.c renders it (half resolution, upscaled), at evenly spaced
 * points of the flythrough; and, for the full bake at the flythrough's
 * start, where one core's draw spends its time stage by stage, whether its
 * two halves slow each other on both cores, and what an empty frame costs.
 * The panel transfer is not included.
 *
 * Runs under DEVICE_BUILD only - needs PSRAM, core 1 and a clock.
 */
#include "suites.h" /* portable - needed by SUITE_REGISTER() even on host */

#ifdef DEVICE_BUILD

#include <stdint.h>
#include <string.h>

#include "unity.h"

#include "esp_log.h"

#include "apps/render_lab/render_lab_view.h"
#include "apps/render_lab/sponza_content.h"
#include "asset/asset_store.h"
#include "gfx/gfx.h"
#include "render/r3d.h"
#include "render/r3d_pipeline.h"
#include "render/r3d_span_internal.h"
#include "render/raster_motion.h"
#include "scene/scene.h"
#include "sponza_suite.h"
#include "util/runtime/job.h"
#include "util/runtime/memory.h"
#include "util/runtime/timing.h"

static const char* TAG = "sponza_perf";

/* The scene, loaded for the suite, its pack, and one mesh for each bake. */
static scene_t* sponza;
static const asset_pack_t* pack;
static const r3d_scene_camera_t* flythrough;
static r3d_lit_mesh_t meshes[SPONZA_BAKE_COUNT];

#define PANEL_PIXELS ((size_t)GFX_WIDTH * GFX_HEIGHT)

static int
render_width(void) {
    return GFX_WIDTH * 100 / render_lab_scale();
}

static int
render_height(void) {
    return GFX_HEIGHT * 100 / render_lab_scale();
}

typedef struct {
    void* scratch;
    gfx_color_t* panel;
    raster_t raster;
} bench_t;

static void
bench_open(bench_t* b, const r3d_instance_t* instance, const raster_attachment_t* const* attachments) {
    b->panel = memory_alloc(sizeof(gfx_color_t) * PANEL_PIXELS, MEMORY_PSRAM);
    b->raster = (raster_t){
        .instances = instance,
        .instance_count = 1,
        .width = render_width(),
        .height = render_height(),
        .upscaled = true,
        .destination = b->panel,
        .destination_width = GFX_WIDTH,
        .destination_height = GFX_HEIGHT,
        .attachments = attachments,
        .attachment_count = attachments == NULL ? 0 : 1,
    };
    b->scratch = memory_alloc(raster_scratch_bytes(&b->raster), MEMORY_PSRAM);
    TEST_ASSERT_NOT_NULL(b->scratch);
    TEST_ASSERT_NOT_NULL(b->panel);
    b->raster.scratch = b->scratch;
    ESP_LOGI(TAG, "heap after setup: internal free %u largest %u, psram free %u largest %u",
             (unsigned)memory_free_bytes(MEMORY_INTERNAL), (unsigned)memory_largest_block(MEMORY_INTERNAL),
             (unsigned)memory_free_bytes(MEMORY_PSRAM), (unsigned)memory_largest_block(MEMORY_PSRAM));
}

static void
bench_close(bench_t* b) {
    memory_free(b->scratch);
    memory_free(b->panel);
}

static r3d_lens_t
view_at(const r3d_lit_mesh_t* mesh, uint32_t t_ms) {
    const camera_t camera = r3d_scene_camera_at(flythrough, t_ms);
    r3d_lens_t lens;
    r3d_lens_init(&lens, &camera, mesh->position_scale, (viewport_t){render_width(), render_height(), 0});
    return lens;
}

static void
clear_and_draw(const raster_t* raster, const r3d_lens_t* lens, int visible) {
    const r3d_pipeline_buffers_t b = r3d_pipeline_carve(raster);
    const size_t pixels = (size_t)raster->width * (size_t)raster->height;
    gfx_color_t* color = gfx_render_target_color(&b.picture, 0);
    for (size_t i = 0; i < pixels; i++) {
        color[i] = raster->clear;
    }
    memset(gfx_render_target_depth(&b.picture, 0), 0, pixels * sizeof(uint16_t));
    const r3d_span_target_t target = {b.picture, NULL, 0};
    r3d_pipeline_draw(raster->instances[0].mesh, lens, b.visible, visible, b.cs, b.rows, &target);
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
    const r3d_pipeline_buffers_t b = r3d_pipeline_carve(f);
    const r3d_span_target_t target = {gfx_render_target_window(&b.picture, j->row0, j->row1), NULL, 0};
    memset(gfx_render_target_depth(&target.rows, j->row0), 0,
           (size_t)(j->row1 - j->row0) * (size_t)f->width * sizeof(uint16_t));
    r3d_pipeline_draw(f->instances[0].mesh, j->lens, b.visible, j->visible, b.cs, b.rows, &target);
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
    ESP_LOGI(TAG, "contention: alone  top %6lldus  bottom %6lldus", (long long)top.us, (long long)bottom.us);
    half_job_t together_top = top;
    half_job_t together_bottom = bottom;
    const int64_t start = timing_now_us();
    (void)job_run_core1(draw_half_on_core1, &together_top, sizeof together_top);
    draw_half(&together_bottom);
    TEST_ASSERT_TRUE(job_wait(1000));
    const int64_t wall = timing_now_us() - start;
    ESP_LOGI(TAG, "contention: at once top %6lldus  bottom %6lldus  wall %6lldus", (long long)core1_result.us,
             (long long)together_bottom.us, (long long)wall);
}

static void
open_the_meshes(void) {
    TEST_ASSERT_NOT_NULL_MESSAGE(sponza, "scene sponza did not load: see the log above");
    for (int i = 0; i < (int)SPONZA_BAKE_COUNT; i++) {
        const scene_entity_t bake = scene_find(sponza, sponza_bakes[i]);
        TEST_ASSERT_NOT_EQUAL_MESSAGE(SCENE_ENTITY_NONE, bake, sponza_bakes[i]);
        const char* mesh = scene_entity_mesh_id(sponza, bake);
        TEST_ASSERT_NOT_NULL_MESSAGE(mesh, sponza_bakes[i]);
        TEST_ASSERT_EQUAL_INT_MESSAGE(ASSET_OK, r3d_lit_mesh_open(pack, mesh, &meshes[i]), mesh);
    }
}

void
test_sponza_draw_stage_breakdown(void) {
    open_the_meshes();
    bench_t b;
    const r3d_instance_t atrium = {&meshes[SPONZA_BAKE_FULL], NULL};
    bench_open(&b, &atrium, NULL);
    const r3d_lens_t lens = view_at(atrium.mesh, 0);
    const r3d_pipeline_buffers_t parts = r3d_pipeline_carve(&b.raster);
    const int visible = r3d_pipeline_cull(atrium.mesh, &lens, parts.visible);
    int64_t start = timing_now_us();
    r3d_pipeline_transform(atrium.mesh, &lens, parts.visible, visible, parts.cs, parts.rows);
    ESP_LOGI(TAG, "stage, one core: %-22s %7lldus", "transform", (long long)(timing_now_us() - start));

    static const char* const names[4] = {"whole draw", "stop after setup", "stop after rows", "stop after span setup"};
    for (int stop = 0; stop <= 3; stop++) {
        r3d_span_stop_after = stop;
        start = timing_now_us();
        clear_and_draw(&b.raster, &lens, visible);
        ESP_LOGI(TAG, "stage, one core: %-22s %7lldus", names[stop], (long long)(timing_now_us() - start));
    }
    r3d_span_stop_after = 0;
    report_core_contention(&b.raster, &lens, visible);

    /* A view of nothing but sky: what a frame costs before any geometry. */
    const camera_t empty = {{0.0F, 20000.0F, 0.0F}, {0.0F, 1.0F, 0.01F}, 1.0F, 1.0F};
    start = timing_now_us();
    const raster_stats_t none = raster_draw(&b.raster, &empty, 0);
    raster_upscale(&b.raster);
    ESP_LOGI(TAG, "stage, both cores: %-20s %7lldus (%d clusters)", "empty frame", (long long)(timing_now_us() - start),
             none.clusters);

    bench_close(&b);
    TEST_PASS();
}

/* `moving`, when not NULL, is the instance's placement, nudged every frame
 * so an attached motion tags every triangle. */
static void
report_frame_cost(const char* label, const r3d_instance_t* instance, const raster_attachment_t* const* attachments,
                  r3d_placement_t* moving) {
    const r3d_lit_mesh_t* mesh = instance->mesh;
    bench_t b;
    bench_open(&b, instance, attachments);
    ESP_LOGI(TAG, "=== %s FRAME COST (%d tris, %d verts, %d clusters, rendered %dx%d) ===", label, mesh->triangle_count,
             mesh->vertex_count, mesh->cluster_count, render_width(), render_height());
    const uint32_t period = r3d_scene_camera_period_ms(flythrough);
    int64_t frame_sum = 0;
    int64_t worst = 0;
    int samples = 0;
    for (uint32_t t_ms = 0; t_ms < period; t_ms += SPONZA_POSE_EVERY_MS) {
        const camera_t camera = r3d_scene_camera_at(flythrough, t_ms);
        if (moving != NULL) {
            moving->position.x = (t_ms / SPONZA_POSE_EVERY_MS) % 2 == 0 ? 0.0F : 0.01F;
        }
        const int64_t start = timing_now_us();
        const raster_stats_t stats = raster_draw(&b.raster, &camera, 0);
        raster_upscale(&b.raster);
        const int64_t us = timing_now_us() - start;
        ESP_LOGI(TAG, "%s t=%5us clusters=%4d tris=%5d | both cores: frame %7lldus", label, (unsigned)(t_ms / 1000),
                 stats.clusters, stats.triangles, (long long)us);
        frame_sum += us;
        worst = us > worst ? us : worst;
        samples++;
    }
    ESP_LOGI(TAG, "%s both cores: mean %lldus (%.1f fps before present), worst %lldus", label,
             (long long)(frame_sum / samples), 1e6 * samples / (double)frame_sum, (long long)worst);
    bench_close(&b);
}

void
test_sponza_frame_cost_along_the_flythrough(void) {
    /* Cache configuration trades internal RAM for speed, so a frame-cost
     * capture carries what it left. */
    ESP_LOGI(TAG, "internal heap: free %u largest %u", (unsigned)memory_free_bytes(MEMORY_INTERNAL),
             (unsigned)memory_largest_block(MEMORY_INTERNAL));
    open_the_meshes();
    for (int i = 0; i < (int)SPONZA_BAKE_COUNT; i++) {
        const r3d_instance_t instance = {&meshes[i], NULL};
        report_frame_cost(sponza_bakes[i], &instance, NULL, NULL);
    }
    TEST_PASS();
}

/* The full bake with motion vectors attached: the camera's motion alone,
 * then with the mesh moving too, so every triangle also tags its pixels. */
void
test_sponza_frame_cost_with_motion(void) {
    open_the_meshes();
    raster_motion_t* motion = memory_alloc(sizeof(*motion), MEMORY_PSRAM);
    TEST_ASSERT_NOT_NULL(motion);
    *motion = (raster_motion_t){0};
    const raster_attachment_t attachment = raster_motion_attachment(motion);
    const raster_attachment_t* const attached[] = {&attachment};
    const r3d_instance_t still = {&meshes[0], NULL};
    report_frame_cost("motion", &still, attached, NULL);
    raster_motion_forget(motion);
    r3d_placement_t placement = {{{1.0F, 0.0F, 0.0F}, {0.0F, 1.0F, 0.0F}, {0.0F, 0.0F, 1.0F}}, {0.0F, 0.0F, 0.0F}};
    const r3d_instance_t moving = {&meshes[0], &placement};
    report_frame_cost("motion_moving", &moving, attached, &placement);
    report_frame_cost("placed", &moving, NULL, &placement);
    memory_free(motion);
    TEST_PASS();
}

void
run_sponza_perf_suite(void) {
    sponza_suite_t loaded = sponza_suite_load();
    sponza = loaded.scene;
    pack = loaded.pack;
    flythrough = loaded.path;
    RUN_TEST(test_sponza_draw_stage_breakdown);
    RUN_TEST(test_sponza_frame_cost_along_the_flythrough);
    RUN_TEST(test_sponza_frame_cost_with_motion);
    loaded = (sponza_suite_t){sponza, pack, flythrough};
    sponza_suite_release(&loaded);
    sponza = NULL;
    pack = NULL;
    flythrough = NULL;
}

#else /* !DEVICE_BUILD */

void
run_sponza_perf_suite(void) {}

#endif /* DEVICE_BUILD */

SUITE_REGISTER(run_sponza_perf_suite);
