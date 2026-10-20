#include "suites.h"
#ifdef DEVICE_BUILD
#include <stdio.h>
#include "apps/render_lab/render_lab.h"
#include "apps/render_lab/render_lab_view.h"
#include "apps/render_lab/tests/r3d_perf_quarters.h"
#include "apps/render_lab/tests/render_lab_test.h"
#include "asset/asset_pack.h"
#include "core/memory.h"
#include "core/timing.h"
#include "esp_log.h"
#include "gfx/gfx.h"
#include "gfx/present/gfx_debug.h"
#include "gfx/present/gfx_mode.h"
#include "gfx/present/gfx_present.h"
#include "perf_stats.h"
#include "render/context/render_context.h"
#include "render/raster_split.h"
#include "scene/scene_shell.h"
#include "services/tune.h"
#include "test_cleanup.h"
#include "ui/ui.h"
#include "unity.h"

/* Each layout replays the same passes from enter, so its last frame is
 * the same pose and a matching checksum of what reached the panel says the
 * layouts look identical. */
#define LAYOUT_WARMUP  4
#define LAYOUT_SAMPLES 120
#define LAYOUT_DT_MS   100

static const char* TAG = "layout_perf";

static const char* const scene_keys[] = {
    "sponza-fitted-full", "sponza", "sponza-lite", "sponza-flat", "sponza-fitted", "sponza-flat-fitted", "capybara",
};
#define SCENE_KEY_COUNT ((int)(sizeof scene_keys / sizeof scene_keys[0]))

static const char* const layout_names[RENDER_LAB_LAYOUT_COUNT] = {"full", "double", "double_clear"};

typedef struct {
    int32_t frame_us[LAYOUT_SAMPLES];
    int32_t wait_us[LAYOUT_SAMPLES];
    int32_t send_us[LAYOUT_SAMPLES];
    int32_t draw_us[LAYOUT_SAMPLES];
    int32_t compose_us[LAYOUT_SAMPLES];
    render_lab_test_t saved;
    int32_t saved_layout;
    bool entered;
    bool primed;
} fixture_t;

static fixture_t* active;

static void
reply(const char* line) {
    ESP_LOGI(TAG, "%s", line);
}

static void
set_layout(int32_t layout) {
    char command[sizeof("SET render_lab.layout -2147483648")];
    TEST_ASSERT_TRUE(snprintf(command, sizeof command, "SET render_lab.layout %ld", (long)layout) > 0);
    TEST_ASSERT_TRUE(tune_handle_line(command, reply));
}

static void
leave_app(fixture_t* f) {
    if (f->entered) {
        render_lab_test_exit(&f->saved);
        f->entered = false;
    }
}

static void
release_fixture(void) {
    leave_app(active);
    set_layout(active->saved_layout);
    memory_free(active);
    active = NULL;
}

static fixture_t*
fixture(void) {
    active = memory_alloc(sizeof *active, MEMORY_PSRAM);
    TEST_ASSERT_NOT_NULL(active);
    *active = (fixture_t){0};
    active->saved_layout = (int32_t)render_lab_layout();
    suite_set_test_cleanup(release_fixture);
    return active;
}

static void
enter_app(fixture_t* f, const char* scene, int quarter) {
    render_lab_test_enter(&f->saved, scene);
    f->entered = true;
    f->primed = false;
    render_lab_test_hold_quarter(quarter);
}

typedef struct {
    int64_t frame_us;
    int64_t draw_us;
    int64_t compose_us;
} pass_t;

/* The shell's pass for an app with update(): last pass's frame is sent
 * while this one updates and renders, then composed and drawn. */
static pass_t
pass(fixture_t* f) {
    const input_t input = {0};
    pass_t took = {0};
    const int64_t start = timing_now_us();
    if (f->primed) {
        gfx_present_begin();
        app_render_lab.update(LAYOUT_DT_MS, &input);
        const int64_t render_start = timing_now_us();
        scene_shell_render(LAYOUT_DT_MS);
        took.draw_us = timing_now_us() - render_start;
        gfx_present_wait();
    }
    ui_clear_band_overlay();
    const int64_t compose_start = timing_now_us();
    scene_shell_compose(LAYOUT_DT_MS);
    took.compose_us = timing_now_us() - compose_start;
    app_render_lab.frame(LAYOUT_DT_MS, &input);
    gfx_band_run(app_render_lab.draw_band, ui_replay_band);
    f->primed = true;
    took.frame_us = timing_now_us() - start;
    return took;
}

/* The last pass's frame as the panel gets it. Double holds it in the back
 * buffer until the next begin, so only double sends it first, with
 * clear-on-send off so the readback can keep it; the others read it as is. */
static uint32_t
sent_frame_crc(fixture_t* f) {
    if (gfx_mode_current()->layout == GFX_LAYOUT_DOUBLE_FB) {
        (void)gfx_set_clear_on_send(false, 0);
        gfx_present_begin();
    }
    gfx_readback_t state = gfx_readback_begin();
    for (int tries = 0; state == GFX_READBACK_PENDING && tries < LAYOUT_WARMUP; tries++) {
        gfx_readback_end();
        (void)pass(f);
        state = gfx_readback_begin();
    }
    TEST_ASSERT_EQUAL_INT(GFX_READBACK_READY, state);
    gfx_color_t* panel = memory_alloc(GFX_FRAMEBUFFER_BYTES, MEMORY_PSRAM);
    TEST_ASSERT_NOT_NULL(panel);
    for (int y = 0; y < GFX_HEIGHT; y++) {
        gfx_read_panel_row(y, &panel[(size_t)y * GFX_WIDTH]);
    }
    gfx_readback_end();
    const uint32_t crc = asset_crc32(panel, GFX_FRAMEBUFFER_BYTES);
    memory_free(panel);
    return crc;
}

static void
report_row(const char* scene, const char* layout, const char* stage, int32_t* samples, const char* quarter) {
    const perf_stats_t stats = perf_stats_compute(samples, LAYOUT_SAMPLES);
    ESP_LOGI(TAG, "%s:%s:%s mean %lldus p95 %lldus" R3D_PERF_QUARTER_FORMAT, scene, layout, stage, (long long)stats.avg,
             (long long)stats.p95, quarter);
}

static void
measure(fixture_t* f, const char* scene, int layout, const r3d_perf_quarter_t* quarter) {
    set_layout(layout);
    enter_app(f, scene, quarter->quarter);
    TEST_ASSERT_EQUAL_INT(layout, render_lab_layout());
    for (int i = -LAYOUT_WARMUP; i < LAYOUT_SAMPLES; i++) {
        const pass_t took = pass(f);
        if (i < 0) {
            continue;
        }
        f->frame_us[i] = (int32_t)took.frame_us;
        f->wait_us[i] = gfx_get_present_wait_us();
        f->send_us[i] = gfx_get_present_send_us();
        f->draw_us[i] = (int32_t)took.draw_us;
        f->compose_us[i] = (int32_t)took.compose_us;
    }
    /* The top slice is core 1's; its share falls when the send contends. */
    const int core1_share = RASTER_SPLIT_EVEN + render_context_main()->split.top_offset[0];
    const unsigned internal_free = (unsigned)memory_free_bytes(MEMORY_INTERNAL);
    const unsigned internal_largest = (unsigned)memory_largest_block(MEMORY_INTERNAL);
    const unsigned psram_free = (unsigned)memory_free_bytes(MEMORY_PSRAM);
    const unsigned psram_largest = (unsigned)memory_largest_block(MEMORY_PSRAM);
    const uint32_t crc = sent_frame_crc(f);
    const char* name = layout_names[layout];
    report_row(scene, name, "frame", f->frame_us, quarter->name);
    report_row(scene, name, "wait", f->wait_us, quarter->name);
    report_row(scene, name, "send", f->send_us, quarter->name);
    report_row(scene, name, "draw", f->draw_us, quarter->name);
    report_row(scene, name, "compose", f->compose_us, quarter->name);
    ESP_LOGI(TAG,
             "LAYOUT scene=%s layout=%s core1_share=%d crc=%08x internal_free=%u internal_largest=%u "
             "psram_free=%u psram_largest=%u" R3D_PERF_QUARTER_FORMAT,
             scene, name, core1_share, (unsigned)crc, internal_free, internal_largest, psram_free, psram_largest,
             quarter->name);
    leave_app(f);
}

static void
test_layout_frame_cost(void) {
    fixture_t* f = fixture();
    for (int s = 0; s < SCENE_KEY_COUNT; s++) {
        for (size_t q = 0; q < R3D_PERF_QUARTER_COUNT; q++) {
            for (int layout = 0; layout < RENDER_LAB_LAYOUT_COUNT; layout++) {
                measure(f, scene_keys[s], layout, &r3d_perf_quarters[q]);
            }
        }
    }
}
#endif

void
run_render_lab_layout_perf_suite(void) {
#ifdef DEVICE_BUILD
    RUN_TEST(test_layout_frame_cost);
#endif
}

SUITE_REGISTER(run_render_lab_layout_perf_suite);
