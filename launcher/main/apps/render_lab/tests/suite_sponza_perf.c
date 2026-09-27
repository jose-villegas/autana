/*
 * Device-only suite: what one Sponza frame costs, the way scene_sponza.c
 * renders it (half resolution, doubled), at evenly spaced points of the
 * flythrough; and, at the heaviest point, where one core's draw spends its
 * time stage by stage. The panel transfer is not included.
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

#include "apps/render_lab/lit_frame.h"
#include "apps/render_lab/sponza_flythrough.h"
#include "apps/render_lab/sponza_mesh_generated.h"
#include "gfx/gfx.h"
#include "util/job.h"

static const char* TAG = "sponza_perf";

#define SAMPLE_EVERY_MS 5000
#define HALF_FOV        0.62f
#define NEAR_Z          6.0f
#define RENDER_WIDTH    (GFX_WIDTH / 2)
#define RENDER_HEIGHT   (GFX_HEIGHT / 2)
#define RENDER_PIXELS   ((size_t)RENDER_WIDTH * RENDER_HEIGHT)
#define PANEL_PIXELS    ((size_t)GFX_WIDTH * GFX_HEIGHT)

typedef struct {
    void* scratch;
    void* target;
    gfx_color_t* panel;
    lit_frame_t frame;
} bench_t;

static void
bench_open(bench_t* b) {
    b->scratch = heap_caps_malloc(lit_frame_scratch_bytes(&sponza_mesh), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    b->target =
        heap_caps_malloc(RENDER_PIXELS * (sizeof(gfx_color_t) + sizeof(uint16_t)), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    b->panel = heap_caps_malloc(sizeof(gfx_color_t) * PANEL_PIXELS, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    TEST_ASSERT_NOT_NULL(b->scratch);
    TEST_ASSERT_NOT_NULL(b->target);
    TEST_ASSERT_NOT_NULL(b->panel);
    gfx_color_t* color = b->target;
    b->frame = (lit_frame_t){
        .mesh = &sponza_mesh,
        .color = color,
        .depth = (uint16_t*)(color + RENDER_PIXELS),
        .width = RENDER_WIDTH,
        .height = RENDER_HEIGHT,
        .doubled = b->panel,
    };
    lit_frame_use_scratch(&b->frame, b->scratch);
}

static void
bench_close(bench_t* b) {
    heap_caps_free(b->scratch);
    heap_caps_free(b->target);
    heap_caps_free(b->panel);
}

static lit_view_t
view_at(uint32_t t_ms) {
    lit_vec3_t eye, forward;
    camera_path_sample(&sponza_flythrough, t_ms, &eye, &forward);
    lit_view_t view;
    lit_view_look(&view, eye, forward, HALF_FOV, NEAR_Z, SPONZA_POSITION_SCALE, RENDER_WIDTH, RENDER_HEIGHT, 0);
    return view;
}

static void
clear_and_draw(const lit_frame_t* frame, const lit_view_t* view, int visible) {
    const size_t pixels = (size_t)frame->width * (size_t)frame->height;
    for (size_t i = 0; i < pixels; i++) {
        frame->color[i] = frame->clear;
    }
    memset(frame->depth, 0, pixels * sizeof(*frame->depth));
    const span_target_t target = {frame->color, frame->depth, frame->width, 0, frame->height};
    lit_draw(frame->mesh, view, frame->visible, visible, frame->cs, frame->rows, &target);
}

typedef struct {
    const lit_frame_t* frame;
    const lit_view_t* view;
    int visible, row0, row1;
    int64_t us;
} half_job_t;

static half_job_t core1_result;

static void
draw_half(half_job_t* j) {
    const lit_frame_t* f = j->frame;
    const size_t offset = (size_t)j->row0 * (size_t)f->width;
    const int64_t start = esp_timer_get_time();
    memset(f->depth + offset, 0, (size_t)(j->row1 - j->row0) * (size_t)f->width * sizeof(uint16_t));
    const span_target_t target = {f->color + offset, f->depth + offset, f->width, j->row0, j->row1};
    lit_draw(f->mesh, j->view, f->visible, j->visible, f->cs, f->rows, &target);
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
report_core_contention(const lit_frame_t* frame, const lit_view_t* view, int visible) {
    const int mid = frame->height / 2;
    half_job_t top = {frame, view, visible, 0, mid, 0}, bottom = {frame, view, visible, mid, frame->height, 0};
    draw_half(&top);
    draw_half(&bottom);
    ESP_LOGI(TAG, "contention: alone  top %6lldus  bottom %6lldus", (long long)top.us, (long long)bottom.us);
    half_job_t together_top = top, together_bottom = bottom;
    const int64_t start = esp_timer_get_time();
    (void)job_run_core1(draw_half_on_core1, &together_top, sizeof together_top);
    draw_half(&together_bottom);
    (void)job_wait(1000);
    const int64_t wall = esp_timer_get_time() - start;
    ESP_LOGI(TAG, "contention: at once top %6lldus  bottom %6lldus  wall %6lldus", (long long)core1_result.us,
             (long long)together_bottom.us, (long long)wall);
}

void
test_sponza_draw_stage_breakdown(void) {
    bench_t b;
    bench_open(&b);
    const lit_view_t view = view_at(0);
    const int visible = lit_cull_clusters(&sponza_mesh, &view, b.frame.visible);
    int64_t start = esp_timer_get_time();
    lit_transform(&sponza_mesh, &view, b.frame.visible, visible, b.frame.cs, b.frame.rows);
    ESP_LOGI(TAG, "stage, one core: %-22s %7lldus", "transform", (long long)(esp_timer_get_time() - start));

    static const char* const names[4] = {"whole draw", "stop after setup", "stop after rows", "stop after span setup"};
    for (int stop = 0; stop <= 3; stop++) {
        span_raster_stop_after = stop;
        start = esp_timer_get_time();
        clear_and_draw(&b.frame, &view, visible);
        ESP_LOGI(TAG, "stage, one core: %-22s %7lldus", names[stop], (long long)(esp_timer_get_time() - start));
    }
    span_raster_stop_after = 0;
    report_core_contention(&b.frame, &view, visible);

    /* A view of nothing but sky: what a frame costs before any geometry. */
    lit_view_t empty;
    lit_view_look(&empty, (lit_vec3_t){0.0f, 20000.0f, 0.0f}, (lit_vec3_t){0.0f, 1.0f, 0.01f}, HALF_FOV, NEAR_Z,
                  SPONZA_POSITION_SCALE, RENDER_WIDTH, RENDER_HEIGHT, 0);
    start = esp_timer_get_time();
    const lit_frame_stats_t none = lit_frame_render(&b.frame, &empty);
    ESP_LOGI(TAG, "stage, both cores: %-20s %7lldus (%d clusters)", "empty frame",
             (long long)(esp_timer_get_time() - start), none.clusters);

    bench_close(&b);
    TEST_PASS();
}

void
test_sponza_frame_cost_along_the_flythrough(void) {
    bench_t b;
    bench_open(&b);
    ESP_LOGI(TAG, "=== SPONZA FRAME COST (%d tris, %d verts, %d clusters, rendered %dx%d) ===", SPONZA_TRIANGLE_COUNT,
             SPONZA_VERTEX_COUNT, SPONZA_CLUSTER_COUNT, RENDER_WIDTH, RENDER_HEIGHT);

    const uint32_t period = camera_path_period_ms(&sponza_flythrough);
    int64_t frame_sum = 0, worst = 0;
    int samples = 0;
    for (uint32_t t_ms = 0; t_ms < period; t_ms += SAMPLE_EVERY_MS) {
        const lit_view_t view = view_at(t_ms);
        const int64_t start = esp_timer_get_time();
        const lit_frame_stats_t stats = lit_frame_render(&b.frame, &view);
        const int64_t us = esp_timer_get_time() - start;
        ESP_LOGI(TAG, "t=%5us clusters=%4d tris=%5d | both cores: frame %7lldus", (unsigned)(t_ms / 1000),
                 stats.clusters, stats.triangles, (long long)us);
        frame_sum += us;
        worst = us > worst ? us : worst;
        samples++;
    }
    ESP_LOGI(TAG, "both cores: mean %lldus (%.1f fps before present), worst %lldus", (long long)(frame_sum / samples),
             1e6 * samples / (double)frame_sum, (long long)worst);

    bench_close(&b);
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
