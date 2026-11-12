/*
 * The frame-time profile of each fitted Sponza, Bistro and capybara scenario, on the
 * board only: every screen quarter against its pegged budget, and on request
 * every scenario at every dynamic-resolution ladder step.
 */

#include "suites.h"
#ifdef DEVICE_BUILD
#include <stdarg.h>
#include <stdio.h>
#include "apps/render_lab/baked_families.h"
#include "apps/render_lab/capybara_content.h"
#include "apps/render_lab/render_lab.h"
#include "apps/render_lab/render_lab_dynres.h"
#include "apps/render_lab/render_lab_view.h"
#include "apps/render_lab/scene_baked.h"
#include "apps/render_lab/sponza_content.h"
#include "apps/render_lab/tests/render_lab_test.h"
#include "collections/strbuf.h"
#include "core/memory.h"
#include "core/timing.h"
#include "core/tune.h"
#include "esp_log.h"
#include "frame/test_frame.h"
#include "frame_profile.h"
#include "lab/lab_scene.h"
#include "lab/test/lab_perf_quarters.h"
#include "lab/test/lab_test.h"
#include "perf_budget.h"
#include "profile/frame_cost.h"
#include "render/context/render_context.h"
#include "render/view/raster_cull_view.h"
#include "scene/animator/animator.h"
#include "testkit/cleanup.h"
#include "unity.h"

/* A capybara row: its take's name under the scene's, the view it is seen
 * from and how the gallop comes in. A cut times one gallop cycle; a blend
 * times only the blend, so every timed frame samples and mixes two clips. */
typedef struct {
    const char* row;
    const char* name;
    const char* view;
    int blend_ms;
} capybara_take_t;

typedef struct {
    const lab_scene_t* scene;
    const char* pack;
    const char* entity;
    const capybara_take_t* take;
    perf_budget_t quarters[LAB_PERF_QUARTER_COUNT];
} profile_scenario_t;

static const capybara_take_t capybara_home = {CAPYBARA_SCENE, CAPYBARA_VIEW_HOME, CAPYBARA_VIEW_HOME,
                                              CAPYBARA_CLIP_CUT_MS};
static const capybara_take_t capybara_close = {"capybara-close", CAPYBARA_VIEW_CLOSE, CAPYBARA_VIEW_CLOSE,
                                               CAPYBARA_CLIP_CUT_MS};
static const capybara_take_t capybara_blend = {"capybara-blend", "blend", CAPYBARA_VIEW_HOME, CAPYBARA_CLIP_BLEND_MS};

/* A capybara scenario: its take and budgets. */
#define CAPYBARA_ROW(take, ...) {&scene_capybara, CAPYBARA_SCENE, CAPYBARA_SUBJECT, &(take), __VA_ARGS__}

/* Worst of a perf-scoped 5-seed capture on A308, 20E8 and 2124, + 1%; per
 * quarter, portrait then landscape. The table's order is the docs' row order. */
static const profile_scenario_t profile_scenarios[] = {
    {.scene = &scene_sponza[SCENE_BAKED_FITTED_FULL], .quarters = {{48826, 60160}, {46066, 56159}}},
    {.scene = &scene_sponza[SCENE_BAKED_FITTED], .quarters = {{40510, 48377}, {38679, 45264}}},
    {.scene = &scene_sponza[SCENE_BAKED_FLAT_FITTED], .quarters = {{39384, 49012}, {37997, 46878}}},
    CAPYBARA_ROW(capybara_home, {{20531, 20793}, {21164, 21432}}),
    CAPYBARA_ROW(capybara_close, {{22108, 22455}, {23064, 23340}}),
    CAPYBARA_ROW(capybara_blend, {{21079, 21407}, {21735, 22015}}),
    {&scene_bistro[SCENE_BAKED_FITTED_FULL], BISTRO_SCENE, BISTRO_CAMERA, NULL, {{36375, 54681}, {38062, 51881}}},
};

static const char* TAG = "render_lab_profile";

/* The steps capture: a line per FRAME_PROFILE_LINE_FRAMES frames of one
 * scenario at one ladder step, then each frame's whole:draw:upscale us and
 * triangles. Denser than real time would be on the slowest steps, so a
 * replay sees the load change at least as fast as a live frame would. */
#define DYNRES_STEPS_FRAME_DT_MS 50
#define DYNRES_STEPS_LINE        "dynres_steps: %s step=%d %dx%d dt=%dms first=%d n=%d"
#define DYNRES_STEPS_FRAME       " %d:%d:%d:%d"
/* The line's head with the longest key and numbers, and one frame's widest fields. */
#define DYNRES_STEPS_LINE_BYTES  (sizeof DYNRES_STEPS_LINE + 64)
#define DYNRES_STEPS_FRAME_BYTES sizeof(" -2147483648:-2147483648:-2147483648:-2147483648")

static lab_test_t saved;
static frame_profile_trace_t played; /* the row playing now, freed with the fixture */

static void
release_fixture(void) {
    frame_profile_trace_free(&played);
    render_lab_test_show_view(RENDER_DEBUG_VIEW_SHADED);
    lab_test_exit(&saved);
}

static int64_t
whole_frame(void* ctx, int64_t dt_us) {
    int64_t* remainder_us = ctx;
    const int64_t step_us = dt_us + *remainder_us;
    const uint32_t dt_ms = (uint32_t)(step_us / MATH_US_PER_MS);
    *remainder_us = step_us % MATH_US_PER_MS;
    const int64_t start = timing_now_us();
    (void)lab_test_frame_dt(dt_ms, false, NULL);
    return timing_now_us() - start;
}

/* The scenario and quarter the next test plays: one test each, so a row over its
 * ceiling does not stop the rows after it. */
static const profile_scenario_t* scenario;
static size_t quarter_index;

/* The row the scenario reports as. */
static const char*
scenario_row(void) {
    return scenario->take == NULL ? scenario->scene->key : scenario->take->row;
}

static const char*
scenario_take(void) {
    return scenario->take == NULL ? NULL : scenario->take->name;
}

/* The scenario's path as the render lab shows it, in the current quarter, its clip from the start. */
typedef struct {
    entity_t scene;
    entity_t entity;
    uint32_t duration_ms;
} scenario_path_t;

/* report_path()'s heap, freed after a failed assertion too. */
static frame_profile_peak_t* peaks;

static void
release_report(void) {
    memory_free(peaks);
    peaks = NULL;
}

/* The played profile, its traced frames and peaks, then each replayed pose:
 * evenly along the path for the counts' curves, then every peak's. A frame
 * of its own, so the test's stays small under the played frames. */
static __attribute__((noinline)) void
report_path(const scenario_path_t* path, const frame_profile_t* p) {
    const char* row = scenario_row();
    const char* quarter = lab_perf_quarters[quarter_index].name;
    frame_profile_log(TAG, row, p, quarter);
    TEST_ASSERT_GREATER_THAN_INT_MESSAGE(0, p->frames, "frame profile failed");
    const int count = frame_profile_peaks(&played, NULL, 0);
    suite_add_test_cleanup(release_report);
    peaks = memory_alloc((size_t)count * sizeof *peaks, MEMORY_APP);
    TEST_ASSERT_NOT_NULL_MESSAGE(peaks, "frame peak allocation failed");
    TEST_ASSERT_EQUAL_INT(count, frame_profile_peaks(&played, peaks, count));
    frame_profile_log_trace(TAG, row, &played, peaks, count, quarter);
    for (int i = 0; i < FRAME_PROFILE_PATH_POSES; i++) {
        render_lab_test_replay_pose(TAG, path->entity, path->duration_ms,
                                    (int32_t)((int64_t)path->duration_ms * i / FRAME_PROFILE_PATH_POSES), row, quarter,
                                    NULL);
    }
    for (int rank = 0; rank < count; rank++) {
        render_lab_test_replay_pose(TAG, path->entity, path->duration_ms, played.frame[peaks[rank].frame].path_ms, row,
                                    quarter, NULL);
    }
    release_report();
}

/* One console line, formatted, for the showing scene. */
static void
send(const char* format, ...) {
    char command[ASSET_NAME_MAX];
    va_list args;
    va_start(args, format);
    const int length = vsnprintf(command, sizeof command, format, args);
    va_end(args);
    TEST_ASSERT_TRUE(length > 0 && (size_t)length < sizeof command);
    TEST_ASSERT_TRUE(app_render_lab.console->handle(command));
}

static scenario_path_t
enter_scenario(void) {
    lab_test_enter(&saved, &render_lab, scenario->scene->key);
    suite_set_test_cleanup(release_fixture);
    test_hold_quarter(lab_perf_quarters[quarter_index].quarter);
    entity_t scene = lab_test_loaded_scene(scenario->pack == NULL ? SPONZA_SCENE : scenario->pack);
    TEST_ASSERT_FALSE(handle_is_none(scene));
    const entity_t entity =
        entity_find(scene, name_hash(scenario->entity == NULL ? sponza_cameras[0] : scenario->entity));
    TEST_ASSERT_FALSE(handle_is_none(entity));
    const capybara_take_t* take = scenario->take;
    if (take != NULL) {
        send(CAPYBARA_VIEW_COMMAND, take->view);
        TEST_ASSERT_TRUE(animator_seek(entity, ANIMATOR_CURRENT, 0));
        send(CAPYBARA_CLIP_COMMAND, take->blend_ms);
    }
    TEST_ASSERT_TRUE(animator_seek(entity, ANIMATOR_CURRENT, 0));
    const int clip = animator_current_clip(entity);
    const uint32_t duration_ms =
        take != NULL && take->blend_ms > 0 ? (uint32_t)take->blend_ms : animator_clip_duration_ms(entity, clip);
    TEST_ASSERT_GREATER_THAN_UINT32(0, duration_ms);
    return (scenario_path_t){scene, entity, duration_ms};
}

/* The played profile against its budget, in a frame of its own for the same reason. */
static __attribute__((noinline)) void
judge_profile(const frame_profile_t* p) {
    lab_perf_budget(scenario_row(), &lab_perf_quarters[quarter_index], p->frame_us.avg, p->frame_us.p95,
                    &scenario->quarters[quarter_index], true);
}

static void
test_real_time_frame_profile(void) {
    const scenario_path_t path = enter_scenario();
    const frame_profile_scenario_t playback = {
        scenario_row(),
        (int64_t)path.duration_ms * MATH_US_PER_MS,
    };
    int64_t remainder_us = 0;
    const frame_profile_t p = frame_profile_play(&playback, whole_frame, &remainder_us, &played);
    /* The peaks first: a row over its budget is the one they explain. */
    report_path(&path, &p);
    judge_profile(&p);
    release_fixture();
    suite_clear_test_cleanup();
}

/* The ladder step the next steps test pins. */
static int ladder_step;

/* One frame of a pinned step: the whole frame, the scaled part's draw and
 * upscale as the context timed them, and the triangles culling kept. */
typedef struct {
    int32_t whole_us, draw_us, upscale_us, triangles;
} step_frame_t;

static __attribute__((noinline)) void
log_step_frames(const step_frame_t* frames, int count, resolution_step_t size) {
    /* Too big for the frame task's stack. */
    const size_t line_size = DYNRES_STEPS_LINE_BYTES + (FRAME_PROFILE_LINE_FRAMES * DYNRES_STEPS_FRAME_BYTES);
    char* line = memory_alloc(line_size, MEMORY_INTERNAL);
    TEST_ASSERT_NOT_NULL(line);
    for (int first = 0; first < count; first += FRAME_PROFILE_LINE_FRAMES) {
        strbuf_t text = strbuf_make(line, line_size);
        const int n = count - first < FRAME_PROFILE_LINE_FRAMES ? count - first : FRAME_PROFILE_LINE_FRAMES;
        strbuf_appendf(&text, DYNRES_STEPS_LINE, scenario_row(), ladder_step, size.width, size.height,
                       DYNRES_STEPS_FRAME_DT_MS, first, n);
        for (int i = first; i < first + n; i++) {
            const step_frame_t* f = &frames[i];
            strbuf_appendf(&text, DYNRES_STEPS_FRAME, (int)f->whole_us, (int)f->draw_us, (int)f->upscale_us,
                           (int)f->triangles);
        }
        ESP_LOGI(TAG, "%s" LAB_PERF_QUARTER_FORMAT, line, lab_perf_quarters[quarter_index].name);
    }
    memory_free(line);
}

/* The path at one ladder step held for every frame, at a fixed frame step so
 * every step draws the same poses: what dynamic resolution would pay for
 * each frame at each step, for launcher/tools/r3d/dynres_replay.py. */
static void
test_dynres_steps(void) {
    const scenario_path_t path = enter_scenario();
    const int frames = (int)((path.duration_ms + DYNRES_STEPS_FRAME_DT_MS - 1) / DYNRES_STEPS_FRAME_DT_MS);
    step_frame_t* records = memory_alloc(sizeof(*records) * (size_t)frames, MEMORY_APP);
    TEST_ASSERT_NOT_NULL(records);
    /* One step and no recovery: the stepped policy holds it and times it. */
    const resolution_step_t size = render_lab_ladder[ladder_step];
    const resolution_config_t pinned = resolution_config(&size, 1, 1, RENDER_LAB_BUDGET_MAX_MS * MATH_US_PER_MS);
    render_context_set_dynamic_resolution(render_context_main(), &pinned, NULL, 0);
    (void)lab_test_frame_dt(0, false, NULL); /* warm-up, not sampled */
    for (int i = 0; i < frames; i++) {
        const int64_t start = timing_now_us();
        (void)lab_test_frame_dt(DYNRES_STEPS_FRAME_DT_MS, false, NULL);
        const int32_t whole_us = (int32_t)(timing_now_us() - start);
        const render_context_frame_t f = render_context_frame(render_context_main());
        TEST_ASSERT_EQUAL_INT(0, f.step);
        records[i] = (step_frame_t){whole_us, f.draw_us, f.upscale_us, f.stats.triangles};
    }
    log_step_frames(records, frames, size);
    memory_free(records);
    release_fixture();
    suite_clear_test_cleanup();
}
#endif

void
run_render_lab_frame_profile_suite(void) {
#ifdef DEVICE_BUILD
    for (size_t i = 0; i < sizeof profile_scenarios / sizeof profile_scenarios[0]; i++) {
        for (quarter_index = 0; quarter_index < LAB_PERF_QUARTER_COUNT; quarter_index++) {
            scenario = &profile_scenarios[i];
            RUN_TEST_CASE(test_real_time_frame_profile, scenario->scene->key, scenario_take(),
                          lab_perf_quarters[quarter_index].name);
        }
    }
#endif
}

SUITE_REGISTER(run_render_lab_frame_profile_suite);

/* On request: every scenario at every ladder step, portrait only. */
void
run_render_lab_dynres_steps_suite(void) {
#ifdef DEVICE_BUILD
    quarter_index = 0;
    for (size_t i = 0; i < sizeof profile_scenarios / sizeof profile_scenarios[0]; i++) {
        scenario = &profile_scenarios[i];
        for (ladder_step = 0; ladder_step < RENDER_LAB_LADDER_STEPS; ladder_step++) {
            RUN_TEST_CASE(test_dynres_steps, scenario->scene->key, scenario_take(),
                          render_lab_test_step_name(ladder_step));
        }
    }
#endif
}

SUITE_REGISTER_ON_REQUEST(run_render_lab_dynres_steps_suite);
