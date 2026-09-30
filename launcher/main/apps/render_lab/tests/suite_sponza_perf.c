/*
 * Device-only suite: what a frame of each Sponza bake costs, the way
 * scene_sponza.c renders it (half resolution, doubled), at evenly spaced
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

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"

#include "apps/render_lab/sponza_flythrough.h"
#include "apps/render_lab/sponza_lite_mesh_generated.h"
#include "apps/render_lab/sponza_mesh_generated.h"
#include "gfx/gfx.h"
#include "render/r3d_lit_frame.h"
#include "util/job.h"

static const char* TAG = "sponza_perf";

#define PANEL_PIXELS ((size_t)GFX_WIDTH * GFX_HEIGHT)

typedef struct {
    void* scratch;
    gfx_color_t* panel;
    r3d_lit_frame_t frame;
} bench_t;

static void
bench_open(bench_t* b, const r3d_lit_mesh_t* mesh) {
    b->panel = heap_caps_malloc(sizeof(gfx_color_t) * PANEL_PIXELS, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    b->frame = (r3d_lit_frame_t){
        .mesh = mesh,
        .width = SPONZA_RENDER_WIDTH,
        .height = SPONZA_RENDER_HEIGHT,
        .doubled = b->panel,
    };
    b->scratch = heap_caps_malloc(r3d_lit_frame_scratch_bytes(mesh, b->frame.width, b->frame.height),
                                  MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    TEST_ASSERT_NOT_NULL(b->scratch);
    TEST_ASSERT_NOT_NULL(b->panel);
    r3d_lit_frame_use_scratch(&b->frame, b->scratch);
}

static void
bench_close(bench_t* b) {
    heap_caps_free(b->scratch);
    heap_caps_free(b->panel);
}

static r3d_lit_view_t
view_at(const r3d_lit_mesh_t* mesh, uint32_t t_ms) {
    r3d_lit_view_t view;
    sponza_view_at(&view, t_ms, mesh->position_scale, 0);
    return view;
}

static void
clear_and_draw(const r3d_lit_frame_t* frame, const r3d_lit_view_t* view, int visible) {
    const size_t pixels = (size_t)frame->width * (size_t)frame->height;
    for (size_t i = 0; i < pixels; i++) {
        frame->color[i] = frame->clear;
    }
    memset(frame->depth, 0, pixels * sizeof(*frame->depth));
    const r3d_span_target_t target = {frame->color, frame->depth, frame->width, 0, frame->height};
    r3d_lit_draw(frame->mesh, view, frame->visible, visible, frame->cs, frame->rows, &target);
}

typedef struct {
    const r3d_lit_frame_t* frame;
    const r3d_lit_view_t* view;
    int visible, row0, row1;
    int64_t us;
} half_job_t;

static half_job_t core1_result;

static void
draw_half(half_job_t* j) {
    const r3d_lit_frame_t* f = j->frame;
    const size_t offset = (size_t)j->row0 * (size_t)f->width;
    const int64_t start = esp_timer_get_time();
    memset(f->depth + offset, 0, (size_t)(j->row1 - j->row0) * (size_t)f->width * sizeof(uint16_t));
    const r3d_span_target_t target = {f->color + offset, f->depth + offset, f->width, j->row0, j->row1};
    r3d_lit_draw(f->mesh, j->view, f->visible, j->visible, f->cs, f->rows, &target);
    j->us = esp_timer_get_time() - start;
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
report_core_contention(const r3d_lit_frame_t* frame, const r3d_lit_view_t* view, int visible) {
    const int mid = frame->height / 2;
    half_job_t top = {frame, view, visible, 0, mid, 0};
    half_job_t bottom = {frame, view, visible, mid, frame->height, 0};
    draw_half(&top);
    draw_half(&bottom);
    ESP_LOGI(TAG, "contention: alone  top %6lldus  bottom %6lldus", (long long)top.us, (long long)bottom.us);
    half_job_t together_top = top;
    half_job_t together_bottom = bottom;
    const int64_t start = esp_timer_get_time();
    (void)job_run_core1(draw_half_on_core1, &together_top, sizeof together_top);
    draw_half(&together_bottom);
    TEST_ASSERT_TRUE(job_wait(1000));
    const int64_t wall = esp_timer_get_time() - start;
    ESP_LOGI(TAG, "contention: at once top %6lldus  bottom %6lldus  wall %6lldus", (long long)core1_result.us,
             (long long)together_bottom.us, (long long)wall);
}

void
test_sponza_draw_stage_breakdown(void) {
    bench_t b;
    bench_open(&b, &sponza_mesh);
    const r3d_lit_view_t view = view_at(&sponza_mesh, 0);
    const int visible = r3d_lit_cull_clusters(&sponza_mesh, &view, b.frame.visible);
    int64_t start = esp_timer_get_time();
    r3d_lit_transform(&sponza_mesh, &view, b.frame.visible, visible, b.frame.cs, b.frame.rows);
    ESP_LOGI(TAG, "stage, one core: %-22s %7lldus", "transform", (long long)(esp_timer_get_time() - start));

    static const char* const names[4] = {"whole draw", "stop after setup", "stop after rows", "stop after span setup"};
    for (int stop = 0; stop <= 3; stop++) {
        r3d_span_stop_after = stop;
        start = esp_timer_get_time();
        clear_and_draw(&b.frame, &view, visible);
        ESP_LOGI(TAG, "stage, one core: %-22s %7lldus", names[stop], (long long)(esp_timer_get_time() - start));
    }
    r3d_span_stop_after = 0;
    report_core_contention(&b.frame, &view, visible);

    /* A view of nothing but sky: what a frame costs before any geometry. */
    r3d_lit_view_t empty;
    r3d_lit_view_look(&empty, (r3d_vec3f_t){0.0F, 20000.0F, 0.0F}, (r3d_vec3f_t){0.0F, 1.0F, 0.01F}, 1.0F, 1.0F,
                      SPONZA_POSITION_SCALE, (r3d_viewport_t){SPONZA_RENDER_WIDTH, SPONZA_RENDER_HEIGHT, 0});
    start = esp_timer_get_time();
    const r3d_lit_stats_t none = r3d_lit_frame_render(&b.frame, &empty);
    r3d_lit_frame_double(&b.frame);
    ESP_LOGI(TAG, "stage, both cores: %-20s %7lldus (%d clusters)", "empty frame",
             (long long)(esp_timer_get_time() - start), none.clusters);

    bench_close(&b);
    TEST_PASS();
}

static void
report_frame_cost(const char* label, const r3d_lit_mesh_t* mesh) {
    bench_t b;
    bench_open(&b, mesh);
    ESP_LOGI(TAG, "=== %s FRAME COST (%d tris, %d verts, %d clusters, rendered %dx%d) ===", label, mesh->triangle_count,
             mesh->vertex_count, mesh->cluster_count, SPONZA_RENDER_WIDTH, SPONZA_RENDER_HEIGHT);
    const uint32_t period = sponza_flythrough_period_ms();
    int64_t frame_sum = 0;
    int64_t worst = 0;
    int samples = 0;
    for (uint32_t t_ms = 0; t_ms < period; t_ms += SPONZA_POSE_EVERY_MS) {
        const r3d_lit_view_t view = view_at(mesh, t_ms);
        const int64_t start = esp_timer_get_time();
        const r3d_lit_stats_t stats = r3d_lit_frame_render(&b.frame, &view);
        r3d_lit_frame_double(&b.frame);
        const int64_t us = esp_timer_get_time() - start;
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
    report_frame_cost("sponza", &sponza_mesh);
    report_frame_cost("lite", &sponza_lite_mesh);
    TEST_PASS();
}

void
run_sponza_perf_suite(void) {
    RUN_TEST(test_sponza_draw_stage_breakdown);
    RUN_TEST(test_sponza_frame_cost_along_the_flythrough);
}

#else /* !DEVICE_BUILD */

void
run_sponza_perf_suite(void) {}

#endif /* DEVICE_BUILD */

SUITE_REGISTER(run_sponza_perf_suite);
