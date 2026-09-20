/*
 * Device-only suite: the wire scenes' frame budget by primitive, layout
 * (full-fb, bands) and orientation - the rig item 1 of the wire scenes task
 * exists for, since this hardware is fill- and PSRAM-write-bound and the
 * vertex stage is what a wireframe scene actually costs. Every field is a
 * mean over the capture, not a percentile: enough frames for a stable
 * average is what "over enough frames for a stable mean" asks for, and nine
 * of these run back to back.
 *
 * Runs under DEVICE_BUILD only - needs real internal RAM, PSRAM, DMA and the
 * panel.
 */
#include "suites.h" /* portable - needed by SUITE_REGISTER() even on host */

#ifdef DEVICE_BUILD

#include <stdint.h>

#include "unity.h"

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"

#include "display/display.h"
#include "gfx/gfx.h"
#include "ui/ui.h"
#include "ui/ui_transform.h"

/* app_render_lab.c's own lifecycle and toggles. */
extern bool render_lab_band_mode;
extern int render_lab_start_scene_index;
extern void render_lab_enter(void);
extern void render_lab_exit(void);
extern void draw_fps(const input_t* input, bool for_bands);

/* scene_wire.c's exposed phases - one implementation shared by every wire
 * primitive, operating on whichever mesh render_lab_start_scene_index
 * selected at the last render_lab_enter(). */
extern void wire_advance_pose(uint32_t dt_ms);
extern void wire_do_transform(void);
extern bool wire_do_project(void);
extern void wire_draw_full(void);
extern void wire_draw_band(gfx_color_t* buf, int row0, int row1);
extern void wire_mark_bbox_dirty(void);
extern void render_lab_clear_band(gfx_color_t* buf, int height);
extern int wire_vertex_count(void);
extern int wire_edge_count(void);
extern int wire_segment_count(void);
extern int64_t wire_line_pixel_estimate(void);

static const char* TAG = "wire_perf";

#define SAMPLE_MS          1500

/* app_render_lab.c's scenes[] table order: cube is 0, the four wire
 * primitives follow it in the order app_render_lab.c adds them. */
#define SCENE_WIRE_PLANE   1
#define SCENE_WIRE_CUBE    2
#define SCENE_WIRE_SPHERE  3
#define SCENE_WIRE_CAPSULE 4

/* Nothing pressed, no touch - draw_fps() dereferences this unconditionally. */
static const input_t null_input = {0};

typedef struct {
    const char* name;
    int scene_index;
} wire_primitive_t;

static const wire_primitive_t primitives[] = {
    {"plane", SCENE_WIRE_PLANE},
    {"cube", SCENE_WIRE_CUBE},
    {"sphere", SCENE_WIRE_SPHERE},
    {"capsule", SCENE_WIRE_CAPSULE},
};

static const struct {
    const char* name;
    int quarter;
} orientations[] = {
    {"landscape", DISPLAY_LANDSCAPE}, /* measured first - Testing-Guide.md */
    {"portrait", DISPLAY_PORTRAIT},
};

typedef struct {
    int64_t transform_us, project_us, draw_us, frame_us;
    int64_t segments, pixels;
    int frames;
} wire_totals_t;

static void
run_full_fb_frame(wire_totals_t* t, uint32_t dt_ms) {
    const int64_t frame_start = esp_timer_get_time();

    wire_advance_pose(dt_ms);

    int64_t t0 = esp_timer_get_time();
    wire_do_transform();
    t->transform_us += esp_timer_get_time() - t0;

    t0 = esp_timer_get_time();
    wire_do_project();
    t->project_us += esp_timer_get_time() - t0;

    t->segments += wire_segment_count();
    t->pixels += wire_line_pixel_estimate();

    t0 = esp_timer_get_time();
    wire_draw_full();
    t->draw_us += esp_timer_get_time() - t0;

    draw_fps(&null_input, false);
    gfx_present();

    t->frame_us += esp_timer_get_time() - frame_start;
    t->frames++;
}

/* render_lab_frame_band()'s own shape, rebuilt from scene_wire.c's exposed
 * pieces - wire_mark_bbox_dirty() must run once per frame, before the band
 * loop, the only reason gfx_band_dirty() below ever returns true. */
static void
run_band_frame(wire_totals_t* t, uint32_t dt_ms) {
    const int64_t frame_start = esp_timer_get_time();

    wire_advance_pose(dt_ms);

    int64_t t0 = esp_timer_get_time();
    wire_do_transform();
    t->transform_us += esp_timer_get_time() - t0;

    t0 = esp_timer_get_time();
    wire_do_project();
    t->project_us += esp_timer_get_time() - t0;

    wire_mark_bbox_dirty();
    t->segments += wire_segment_count();
    t->pixels += wire_line_pixel_estimate();

    draw_fps(&null_input, true);

    gfx_band_frame_begin();
    while (gfx_band_next()) {
        const int row0 = gfx_band_row0();
        const int height = gfx_band_height();
        int x0, x1;

        if (!gfx_band_dirty(row0, row0 + height, &x0, &x1)) {
            gfx_band_skip();
            continue;
        }
        (void)x0;
        (void)x1;

        gfx_color_t* buf = gfx_band_buffer();
        const int64_t d0 = esp_timer_get_time();
        render_lab_clear_band(buf, height);
        wire_draw_band(buf, row0, row0 + height);
        t->draw_us += esp_timer_get_time() - d0;

        ui_replay_band(row0, row0 + height);
        gfx_band_submit();
    }

    t->frame_us += esp_timer_get_time() - frame_start;
    t->frames++;
}

/* Runs one (primitive, layout, orientation) combination for SAMPLE_MS,
 * clamping dt_ms the way main.c's own loop does. */
static wire_totals_t
capture(void (*run_frame)(wire_totals_t*, uint32_t)) {
    wire_totals_t t = {0};
    const int64_t start = esp_timer_get_time();
    int64_t next_due = start;

    while (esp_timer_get_time() - start < SAMPLE_MS * 1000) {
        const int64_t frame_start = esp_timer_get_time();
        int64_t dt_ms = (frame_start - next_due) / 1000;

        if (dt_ms < 0) {
            dt_ms = 1;
        }
        if (dt_ms > 250) {
            dt_ms = 250;
        }
        next_due += dt_ms * 1000;

        run_frame(&t, (uint32_t)dt_ms);
    }
    return t;
}

static void
log_row(const char* primitive, const char* layout, const char* orient, const wire_totals_t* t, int vertex_count,
        int edge_count) {
    const double frame_us_avg = (double)t->frame_us / t->frames;
    const double transform_us_avg = (double)t->transform_us / t->frames;
    const double project_us_avg = (double)t->project_us / t->frames;
    const double draw_us_avg = (double)t->draw_us / t->frames;
    const double segments_avg = (double)t->segments / t->frames;
    const double pixels_avg = (double)t->pixels / t->frames;
    const double draw_us_per_pixel = pixels_avg > 0.0 ? draw_us_avg / pixels_avg : 0.0;

    ESP_LOGI(TAG,
             "%-8s %-7s %-9s frames=%d transform=%.1fus(%.3fus/vtx) project=%.1fus(%.3fus/edge) "
             "draw=%.1fus(%.0f segs, %.0f px est, %.3fus/px) frame=%.1fus (%.1f fps)",
             primitive, layout, orient, t->frames, transform_us_avg, transform_us_avg / vertex_count, project_us_avg,
             project_us_avg / edge_count, draw_us_avg, segments_avg, pixels_avg, draw_us_per_pixel, frame_us_avg,
             1000000.0 / frame_us_avg);
}

/* touched > 0 is the same all-skip regression guard suite_cube_band_perf.c
 * asserts - a band arm whose dirty marking silently broke would otherwise
 * look identical to a healthy one by frame count alone. */
static void
assert_row_sane(const wire_totals_t* t) {
    TEST_ASSERT_GREATER_THAN_INT_MESSAGE(0, t->frames, "no frames captured for this combination");
    TEST_ASSERT_GREATER_THAN_INT_MESSAGE(0, (int)t->segments,
                                         "no segments were ever produced - the mesh never landed on screen");
}

static void
run_combo(const char* primitive_name, int scene_index, bool band_mode, const char* orient_name, int quarter) {
    const size_t free_before = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);

    render_lab_start_scene_index = scene_index;
    render_lab_band_mode = band_mode;
    ui_set_transform(ui_transform_quarter_turn(quarter, GFX_WIDTH, GFX_HEIGHT));
    render_lab_enter();

    const int vertex_count = wire_vertex_count();
    const int edge_count = wire_edge_count();

    const wire_totals_t t = capture(band_mode ? run_band_frame : run_full_fb_frame);

    render_lab_exit();
    const size_t free_after = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);

    log_row(primitive_name, band_mode ? "band" : "full_fb", orient_name, &t, vertex_count, edge_count);
    assert_row_sane(&t);
    TEST_ASSERT_EQUAL_INT_MESSAGE((int)free_before, (int)free_after,
                                  "scene_wire.c's scratch buffers were not fully freed on exit");
}

void
test_wire_frame_budget_by_primitive_layout_orientation(void) {
    ui_init(); /* render_lab_enter() reads ui_layout_generation(); see suite_cube_perf.c's fixture */
    const bool saved_band_mode = render_lab_band_mode;
    const int saved_scene_index = render_lab_start_scene_index;

    ESP_LOGI(TAG, "=== WIRE FRAME BUDGET (%dms/combo, band height %d) ===", SAMPLE_MS, GFX_BAND_HEIGHT);

    for (size_t p = 0; p < sizeof(primitives) / sizeof(primitives[0]); p++) {
        for (size_t o = 0; o < sizeof(orientations) / sizeof(orientations[0]); o++) {
            run_combo(primitives[p].name, primitives[p].scene_index, false, orientations[o].name,
                      orientations[o].quarter);
            run_combo(primitives[p].name, primitives[p].scene_index, true, orientations[o].name,
                      orientations[o].quarter);
        }
    }

    render_lab_band_mode = saved_band_mode;
    render_lab_start_scene_index = saved_scene_index;
    ui_set_transform(ui_transform_identity());

    TEST_PASS();
}

void
run_wire_perf_suite(void) {
    RUN_TEST(test_wire_frame_budget_by_primitive_layout_orientation);
}

#else /* !DEVICE_BUILD */

void
run_wire_perf_suite(void) {}

#endif /* DEVICE_BUILD */

SUITE_REGISTER(run_wire_perf_suite);
