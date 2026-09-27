/*
 * Device-only suite: what one Sponza frame costs, at evenly spaced points of
 * the flythrough. Each point is timed as the scene runs it (lit_frame.h, both
 * cores) and stage by stage on one core, into a private PSRAM canvas; the
 * panel transfer is not included.
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

static const char* TAG = "sponza_perf";

#define SAMPLE_EVERY_MS 5000
#define HALF_FOV        0.62f
#define NEAR_Z          6.0f

typedef struct {
    int64_t cull_us, transform_us, draw_us, quarter_draw_us, frame_us;
} stage_times_t;

static void
clear_and_draw(const lit_frame_t* frame, const lit_view_t* view, int visible, int width, int height) {
    const size_t pixels = (size_t)width * (size_t)height;
    for (size_t i = 0; i < pixels; i++) {
        frame->color[i] = frame->clear;
    }
    memset(frame->depth, 0, pixels * sizeof(*frame->depth));
    const span_target_t target = {frame->color, frame->depth, width, 0, height};
    lit_draw(frame->mesh, view, frame->visible, visible, frame->cs, NULL, &target);
}

/* The same view at half width and half height keeps the same triangles but
 * fills a quarter of the pixels, which separates per-triangle cost from
 * per-pixel cost. */
static stage_times_t
time_point(const lit_frame_t* frame, lit_vec3_t eye, lit_vec3_t forward, lit_frame_stats_t* stats) {
    lit_view_t view_storage;
    lit_view_look(&view_storage, eye, forward, HALF_FOV, NEAR_Z, SPONZA_POSITION_SCALE, GFX_WIDTH, GFX_HEIGHT, 0);
    const lit_view_t* view = &view_storage;
    stage_times_t t;
    int64_t start = esp_timer_get_time();
    const int visible = lit_cull_clusters(frame->mesh, view, frame->visible);
    t.cull_us = esp_timer_get_time() - start;

    start = esp_timer_get_time();
    lit_transform(frame->mesh, view, frame->visible, visible, frame->cs, NULL);
    t.transform_us = esp_timer_get_time() - start;

    start = esp_timer_get_time();
    clear_and_draw(frame, view, visible, frame->width, frame->height);
    t.draw_us = esp_timer_get_time() - start;

    lit_view_t quarter;
    lit_view_look(&quarter, eye, forward, HALF_FOV, NEAR_Z, SPONZA_POSITION_SCALE, GFX_WIDTH / 2, GFX_HEIGHT / 2, 0);
    lit_transform(frame->mesh, &quarter, frame->visible, visible, frame->cs, NULL);
    start = esp_timer_get_time();
    clear_and_draw(frame, &quarter, visible, GFX_WIDTH / 2, GFX_HEIGHT / 2);
    t.quarter_draw_us = esp_timer_get_time() - start;

    start = esp_timer_get_time();
    *stats = lit_frame_render(frame, view);
    t.frame_us = esp_timer_get_time() - start;
    return t;
}

void
test_sponza_frame_cost_along_the_flythrough(void) {
    void* scratch = heap_caps_malloc(lit_frame_scratch_bytes(&sponza_mesh), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    gfx_color_t* color =
        heap_caps_malloc(sizeof(*color) * (size_t)GFX_WIDTH * GFX_HEIGHT, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    uint16_t* depth =
        heap_caps_malloc(sizeof(*depth) * (size_t)GFX_WIDTH * GFX_HEIGHT, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    TEST_ASSERT_NOT_NULL(scratch);
    TEST_ASSERT_NOT_NULL(color);
    TEST_ASSERT_NOT_NULL(depth);

    lit_frame_t frame = {
        .mesh = &sponza_mesh, .color = color, .depth = depth, .width = GFX_WIDTH, .height = GFX_HEIGHT};
    lit_frame_use_scratch(&frame, scratch);
    ESP_LOGI(TAG, "=== SPONZA FRAME COST (%d tris, %d verts, %d clusters) ===", SPONZA_TRIANGLE_COUNT,
             SPONZA_VERTEX_COUNT, SPONZA_CLUSTER_COUNT);

    const uint32_t period = camera_path_period_ms(&sponza_flythrough);
    int64_t frame_sum = 0, worst = 0;
    int samples = 0;
    for (uint32_t t_ms = 0; t_ms < period; t_ms += SAMPLE_EVERY_MS) {
        lit_vec3_t eye, forward;
        camera_path_sample(&sponza_flythrough, t_ms, &eye, &forward);
        lit_frame_stats_t stats;
        const stage_times_t st = time_point(&frame, eye, forward, &stats);
        ESP_LOGI(TAG,
                 "t=%5us clusters=%4d tris=%5d | one core: cull %5lldus transform %6lldus draw %7lldus "
                 "quarter-area draw %7lldus | both cores: frame %7lldus",
                 (unsigned)(t_ms / 1000), stats.clusters, stats.triangles, (long long)st.cull_us,
                 (long long)st.transform_us, (long long)st.draw_us, (long long)st.quarter_draw_us,
                 (long long)st.frame_us);
        frame_sum += st.frame_us;
        worst = st.frame_us > worst ? st.frame_us : worst;
        samples++;
    }
    ESP_LOGI(TAG, "both cores: mean %lldus (%.1f fps before present), worst %lldus", (long long)(frame_sum / samples),
             1e6 * samples / (double)frame_sum, (long long)worst);

    heap_caps_free(scratch);
    heap_caps_free(color);
    heap_caps_free(depth);
    TEST_PASS();
}

void
run_sponza_perf_suite(void) {
    RUN_TEST(test_sponza_frame_cost_along_the_flythrough);
}

#else /* !DEVICE_BUILD */

void
run_sponza_perf_suite(void) {}

#endif /* DEVICE_BUILD */

SUITE_REGISTER(run_sponza_perf_suite);
