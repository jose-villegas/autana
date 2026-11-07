/*
 * On request, board only: partial redraw (render/partial) against a full
 * redraw, the render.partial_redraw knob off then on. Each scenario plays
 * from the same start at a fixed frame step, so both passes draw the same
 * poses: still-camera scenes, where partial redraw should pay, and a moving
 * camera, where it falls back to every row and should cost nothing. Each
 * pass logs its whole-frame time, the bytes the present sent and the panel
 * rows recomposed; every frame's composed picture is checksummed, and the
 * partial pass must match the full one frame for frame.
 */
#include "suites.h"
#ifdef DEVICE_BUILD
#include <stdio.h>
#include "apps/render_lab/capybara_content.h"
#include "apps/render_lab/render_lab_scene.h"
#include "apps/render_lab/scene_baked.h"
#include "apps/render_lab/sponza_content.h"
#include "apps/render_lab/tests/render_lab_test.h"
#include "core/memory.h"
#include "core/timing.h"
#include "core/tune.h"
#include "esp_log.h"
#include "frame/test_frame.h"
#include "gfx/gfx.h"
#include "gfx/present/gfx_debug.h"
#include "gfx/present/gfx_mode.h"
#include "hash/crc32.h"
#include "math/stats/statsi.h"
#include "render/context/render_context.h"
#include "scene/scene_internal.h"
#include "testkit/cleanup.h"
#include "unity.h"

static const char* TAG = "partial_perf";

/* Frames per pass, after the warm-up, and the step between them: a gallop
 * cycle and more, at the panel's frame rate. */
#define PARTIAL_FRAMES      120
#define PARTIAL_WARMUP      8
#define PARTIAL_FRAME_DT_MS 33

typedef struct {
    const char* name;
    const render_lab_scene_t* scene;
    void (*start)(void); /* puts the scenario at its first frame */
} scenario_t;

typedef struct {
    render_lab_test_t saved;
    int32_t frame_us[PARTIAL_FRAMES];
    int32_t sorted_us[PARTIAL_FRAMES]; /* the percentile picks reorder it */
    uint32_t crc[2][PARTIAL_FRAMES];   /* full, then partial */
} fixture_t;

static fixture_t* active;

static void
release_fixture(void) {
    (void)tune_reset(tune_shared(), TUNE_NAME(render, partial_redraw));
    render_lab_test_exit(&active->saved);
    memory_free(active);
    active = NULL;
}

/* The picture the panel shows: the half picture when expanded, else the framebuffer. */
static uint32_t
picture_crc(void) {
    if (gfx_frame_expanded()) {
        return crc32_update(0, gfx_half_picture(), sizeof(gfx_color_t) * (GFX_WIDTH / 2) * (GFX_HEIGHT / 2));
    }
    return crc32_update(0, gfx_framebuffer(), GFX_FRAMEBUFFER_BYTES);
}

static void
send(const char* command) {
    TEST_ASSERT_TRUE(app_render_lab.console->handle(command));
}

/* The capybara galloping on the spot from its first frame, seen from home. */
static void
start_gallop(void) {
    scene_t* scene = render_lab_test_loaded_scene(CAPYBARA_SCENE);
    TEST_ASSERT_NOT_NULL(scene);
    send("view " CAPYBARA_VIEW_HOME);
    char command[ASSET_NAME_MAX];
    TEST_ASSERT_TRUE(snprintf(command, sizeof command, CAPYBARA_CLIP_COMMAND, CAPYBARA_CLIP_CUT_MS) > 0);
    send(command);
    const scene_entity_t subject = scene_find(scene, name_hash(CAPYBARA_SUBJECT));
    TEST_ASSERT_TRUE(scene_animator_seek(scene, subject, SCENE_ANIMATOR_CURRENT, 0));
}

/* The water scene as it opens, its camera still: nothing to set. */
static void
start_water(void) {
    TEST_ASSERT_NOT_NULL(render_lab_test_loaded_scene(scene_water.key));
}

/* The flythrough from its start: the camera moves every frame. */
static void
start_flythrough(void) {
    scene_t* scene = render_lab_test_loaded_scene(SPONZA_SCENE);
    TEST_ASSERT_NOT_NULL(scene);
    const scene_entity_t camera = scene_find(scene, name_hash(sponza_cameras[0]));
    TEST_ASSERT_NOT_EQUAL(SCENE_ENTITY_NONE, camera);
    TEST_ASSERT_TRUE(scene_animator_seek(scene, camera, SCENE_ANIMATOR_CURRENT, 0));
}

static const scenario_t scenarios[] = {
    {"gallop", &scene_capybara, start_gallop},
    {"water", &scene_water, start_water},
    {"flythrough", &scene_sponza[SCENE_BAKED_FITTED_FULL], start_flythrough},
};

/* One pass of `s` from its start, partial redraw on or off. */
static void
play(fixture_t* f, const scenario_t* s, bool partial) {
    TEST_ASSERT_TRUE(tune_set(tune_shared(), TUNE_NAME(render, partial_redraw), partial ? 1 : 0));
    s->start();
    render_context_forget(render_context_main());
    for (int i = 0; i < PARTIAL_WARMUP; i++) {
        (void)render_lab_test_frame_dt(0, false, NULL);
    }
    int64_t rows = 0;
    gfx_reset_strip_send_counts();
    for (int i = 0; i < PARTIAL_FRAMES; i++) {
        const int64_t began = timing_now_us();
        (void)render_lab_test_frame_dt(PARTIAL_FRAME_DT_MS, false, NULL);
        f->frame_us[i] = (int32_t)(timing_now_us() - began);
        f->crc[partial][i] = picture_crc();
        const render_context_frame_t r = render_context_frame(render_context_main());
        rows += r.unchanged ? 0 : r.rows_end - r.rows_first;
    }
    const int64_t bytes = gfx_get_bytes_sent();
    int64_t sum = 0;
    for (int i = 0; i < PARTIAL_FRAMES; i++) {
        f->sorted_us[i] = f->frame_us[i];
        sum += f->frame_us[i];
    }
    const int32_t p50 = statsi_percentile(f->sorted_us, PARTIAL_FRAMES, 50U);
    const int32_t p95 = statsi_percentile(f->sorted_us, PARTIAL_FRAMES, 95U);
    ESP_LOGI(TAG,
             "partial_perf: scenario=%s mode=%s frames=%d frame_avg_us=%ld frame_p50_us=%ld frame_p95_us=%ld "
             "bytes_per_frame=%ld rows_per_frame=%ld",
             s->name, partial ? "partial" : "full", PARTIAL_FRAMES, (long)(sum / PARTIAL_FRAMES), (long)p50, (long)p95,
             (long)(bytes / PARTIAL_FRAMES), (long)(rows / PARTIAL_FRAMES));
}

/* Every scenario plays and logs before any mismatch fails the test. */
static void
test_partial_matches_full(void) {
    int mismatched = 0;
    for (size_t k = 0; k < sizeof scenarios / sizeof scenarios[0]; k++) {
        const scenario_t* s = &scenarios[k];
        active = memory_alloc(sizeof *active, MEMORY_APP);
        TEST_ASSERT_NOT_NULL(active);
        *active = (fixture_t){0};
        render_lab_test_enter(&active->saved, s->scene->key);
        suite_set_test_cleanup(release_fixture);
        test_hold_quarter(0);
        play(active, s, false);
        play(active, s, true);
        int differing = 0;
        for (int i = 0; i < PARTIAL_FRAMES; i++) {
            differing += active->crc[0][i] != active->crc[1][i];
        }
        ESP_LOGI(TAG, "partial_perf: scenario=%s identical_frames=%d of %d", s->name, PARTIAL_FRAMES - differing,
                 PARTIAL_FRAMES);
        mismatched += differing;
        release_fixture();
        suite_clear_test_cleanup();
    }
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, mismatched, "partial redraw changed the picture");
}
#endif

void
run_partial_perf_suite(void) {
#ifdef DEVICE_BUILD
    RUN_TEST(test_partial_matches_full);
#endif
}

SUITE_REGISTER_ON_REQUEST(run_partial_perf_suite);
