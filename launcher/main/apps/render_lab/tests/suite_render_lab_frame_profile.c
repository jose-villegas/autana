#include "suites.h"
#ifdef DEVICE_BUILD
#include <stdarg.h>
#include <stdio.h>
#include "apps/render_lab/baked_families.h"
#include "apps/render_lab/capybara_content.h"
#include "apps/render_lab/render_lab_dynres.h"
#include "apps/render_lab/render_lab_scene.h"
#include "apps/render_lab/render_lab_view.h"
#include "apps/render_lab/scene_baked.h"
#include "apps/render_lab/sponza_content.h"
#include "apps/render_lab/tests/r3d_perf_quarters.h"
#include "apps/render_lab/tests/render_lab_test.h"
#include "collections/strbuf.h"
#include "core/memory.h"
#include "core/timing.h"
#include "core/tune.h"
#include "esp_log.h"
#include "frame_profile.h"
#include "perf_budget.h"
#include "profile/frame_cost.h"
#include "render/context/render_context.h"
#include "render/view/raster_cull_view.h"
#include "scene/scene_internal.h"
#include "testkit/cleanup.h"
#include "unity.h"

/* A capybara row: the view it is seen from and how the gallop comes in. A
 * cut times one gallop cycle; a blend times only the blend, so every timed
 * frame samples and mixes two clips. */
typedef struct {
    const char* row;
    const char* view;
    int blend_ms;
} capybara_take_t;

typedef struct {
    const render_lab_scene_t* scene;
    const char* pack;
    const char* entity;
    const capybara_take_t* take;
    perf_budget_t quarters[R3D_PERF_QUARTER_COUNT];
} profile_scenario_t;

static const capybara_take_t capybara_home = {CAPYBARA_SCENE, CAPYBARA_VIEW_HOME, CAPYBARA_CLIP_CUT_MS};
static const capybara_take_t capybara_front = {"capybara-front", CAPYBARA_VIEW_FRONT, CAPYBARA_CLIP_CUT_MS};
static const capybara_take_t capybara_side = {"capybara-side", CAPYBARA_VIEW_SIDE, CAPYBARA_CLIP_CUT_MS};
static const capybara_take_t capybara_back = {"capybara-back", CAPYBARA_VIEW_BACK, CAPYBARA_CLIP_CUT_MS};
static const capybara_take_t capybara_close = {"capybara-close", CAPYBARA_VIEW_CLOSE, CAPYBARA_CLIP_CUT_MS};
static const capybara_take_t capybara_blend = {"capybara-blend", CAPYBARA_VIEW_HOME, CAPYBARA_CLIP_BLEND_MS};

/* A capybara scenario: its take and budgets. */
#define CAPYBARA_ROW(take, ...) {&scene_capybara, CAPYBARA_SCENE, CAPYBARA_SUBJECT, &(take), __VA_ARGS__}

/* Worst of a 5-seed capture on A308 (before and after its move), 20E8 and
 * 2124, + 1%; per quarter, portrait then landscape. */
static const profile_scenario_t profile_scenarios[] = {
    {.scene = &scene_sponza[SCENE_BAKED_FULL], .quarters = {{48953, 59273}, {47229, 56329}}},
    {.scene = &scene_sponza[SCENE_BAKED_LITE], .quarters = {{41957, 47931}, {40738, 46118}}},
    {.scene = &scene_sponza[SCENE_BAKED_FLAT], .quarters = {{40902, 49780}, {40227, 48295}}},
    {.scene = &scene_sponza[SCENE_BAKED_FITTED], .quarters = {{41341, 49565}, {39726, 46664}}},
    {.scene = &scene_sponza[SCENE_BAKED_FITTED_FULL], .quarters = {{49536, 62165}, {47469, 58108}}},
    {.scene = &scene_sponza[SCENE_BAKED_FLAT_FITTED], .quarters = {{40285, 50300}, {39053, 48179}}},
    CAPYBARA_ROW(capybara_home, {{20941, 21286}, {21500, 21803}}),
    CAPYBARA_ROW(capybara_front, {{20907, 21289}, {21305, 21590}}),
    CAPYBARA_ROW(capybara_side, {{20921, 21268}, {21591, 21909}}),
    CAPYBARA_ROW(capybara_back, {{20352, 20994}, {20989, 21423}}),
    CAPYBARA_ROW(capybara_close, {{23002, 23384}, {23807, 24080}}),
    CAPYBARA_ROW(capybara_blend, {{21806, 22738}, {22438, 22866}}),
    {&scene_bistro[SCENE_BAKED_FITTED_FULL], BISTRO_SCENE, BISTRO_CAMERA, NULL, {{41782, 62092}, {40404, 57696}}},
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

static render_lab_test_t saved;
static frame_profile_trace_t played; /* the row playing now, freed with the fixture */

/* The scene applies render_lab.view after it draws, so the view is also set here for the next draw. */
static void
show_view(int view) {
    TEST_ASSERT_TRUE(tune_set(tune_shared(), TUNE_NAME(render_lab, view), view));
    render_context_set_debug_view(render_context_main(), view);
    TEST_ASSERT_EQUAL_INT(view, render_context_main()->debug_view);
}

static void
release_fixture(void) {
    frame_profile_trace_free(&played);
    show_view(RENDER_DEBUG_VIEW_SHADED);
    render_lab_test_exit(&saved);
}

static int64_t
whole_frame(void* ctx, int64_t dt_us) {
    int64_t* remainder_us = ctx;
    const int64_t step_us = dt_us + *remainder_us;
    const uint32_t dt_ms = (uint32_t)(step_us / MATH_US_PER_MS);
    *remainder_us = step_us % MATH_US_PER_MS;
    const int64_t start = timing_now_us();
    (void)render_lab_test_frame_dt(dt_ms, false, NULL);
    return timing_now_us() - start;
}

/* A replayed pose's log line parts, too big for the stack its draws run on. */
#define COUNT_BYTES(name, member) char name[sizeof(" " #name "=-2147483648")];

typedef struct {
    char counts[sizeof(struct { RASTER_CULL_FRAME_FIELDS(COUNT_BYTES) char end; })];
    char report[FRAME_COST_REPORT_MAX];
} pose_line_t;

/* The cull view's counts of the frame just drawn, in a frame of their own
 * that is gone before the next draw. */
static __attribute__((noinline)) void
write_cull_counts(pose_line_t* out) {
    const raster_cull_t* cull = render_context_debug_view_state(render_context_main());
    TEST_ASSERT_NOT_NULL(cull);
    const raster_cull_frame_t f = raster_cull_frame(cull);
    TEST_ASSERT_GREATER_THAN_INT_MESSAGE(0, f.counts.nodes_visited, "the cull view did not draw");
    strbuf_t line = strbuf_make(out->counts, sizeof out->counts);
#define COUNT_PAIR(name, member) strbuf_appendf(&line, "%s" #name "=%d", line.length == 0 ? "" : " ", f.member);
    RASTER_CULL_FRAME_FIELDS(COUNT_PAIR)
}

/* Draws the pose `path_ms` along the played path, paused there (dt 0, so a skinned
 * subject is not skinned again): one frame through the cull view for its
 * counts, then FRAME_PROFILE_REPLAY_DRAWS shaded frames for their frame-cost
 * report. */
static void
replay_pose(scene_t* scene, scene_entity_t entity, uint32_t duration_ms, int32_t path_ms, const char* row,
            const char* quarter, pose_line_t* out) {
    TEST_ASSERT_TRUE(scene_animator_seek(scene, entity, SCENE_ANIMATOR_CURRENT, (uint32_t)path_ms % duration_ms));
    show_view(render_context_debug_view_named("cull"));
    (void)render_lab_test_frame_dt(0, false, NULL);
    write_cull_counts(out);
    show_view(RENDER_DEBUG_VIEW_SHADED);
    (void)render_lab_test_frame_dt(0, false, NULL);
    (void)frame_cost_take_report(1, out->report, sizeof out->report);
    for (int i = 0; i < FRAME_PROFILE_REPLAY_DRAWS; i++) {
        (void)render_lab_test_frame_dt(0, false, NULL);
    }
    (void)frame_cost_take_report(FRAME_PROFILE_REPLAY_DRAWS, out->report, sizeof out->report);
    ESP_LOGI(TAG, FRAME_PROFILE_REPLAY R3D_PERF_QUARTER_FORMAT, row, (int)path_ms, out->counts, out->report, quarter);
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

/* The scenario's path as the render lab shows it, in the current quarter, its clip from the start. */
typedef struct {
    scene_t* scene;
    scene_entity_t entity;
    uint32_t duration_ms;
} scenario_path_t;

/* report_path()'s heap, freed after a failed assertion too. */
static frame_profile_peak_t* peaks;
static pose_line_t* pose_line;

static void
release_report(void) {
    memory_free(peaks);
    memory_free(pose_line);
    peaks = NULL;
    pose_line = NULL;
}

/* The played profile, its traced frames and peaks, then each replayed pose:
 * evenly along the path for the counts' curves, then every peak's. A frame
 * of its own, so the test's stays small under the played frames. */
static __attribute__((noinline)) void
report_path(const scenario_path_t* path, const frame_profile_t* p) {
    const char* row = scenario_row();
    const char* quarter = r3d_perf_quarters[quarter_index].name;
    frame_profile_log(TAG, row, p, quarter);
    TEST_ASSERT_GREATER_THAN_INT_MESSAGE(0, p->frames, "frame profile failed");
    const int count = frame_profile_peaks(&played, NULL, 0);
    suite_add_test_cleanup(release_report);
    peaks = memory_alloc((size_t)count * sizeof *peaks, MEMORY_PSRAM);
    pose_line = memory_alloc(sizeof *pose_line, MEMORY_INTERNAL);
    TEST_ASSERT_NOT_NULL_MESSAGE(peaks, "frame peak allocation failed");
    TEST_ASSERT_NOT_NULL_MESSAGE(pose_line, "frame peak allocation failed");
    TEST_ASSERT_EQUAL_INT(count, frame_profile_peaks(&played, peaks, count));
    frame_profile_log_trace(TAG, row, &played, peaks, count, quarter);
    for (int i = 0; i < FRAME_PROFILE_PATH_POSES; i++) {
        replay_pose(path->scene, path->entity, path->duration_ms,
                    (int32_t)((int64_t)path->duration_ms * i / FRAME_PROFILE_PATH_POSES), row, quarter, pose_line);
    }
    for (int rank = 0; rank < count; rank++) {
        replay_pose(path->scene, path->entity, path->duration_ms, played.frame[peaks[rank].frame].path_ms, row, quarter,
                    pose_line);
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
    render_lab_test_enter(&saved, scenario->scene->key);
    suite_set_test_cleanup(release_fixture);
    render_lab_test_hold_quarter(r3d_perf_quarters[quarter_index].quarter);
    scene_t* scene = render_lab_test_loaded_scene(scenario->pack == NULL ? SPONZA_SCENE : scenario->pack);
    TEST_ASSERT_NOT_NULL(scene);
    const scene_entity_t entity =
        scene_find(scene, name_hash(scenario->entity == NULL ? sponza_cameras[0] : scenario->entity));
    TEST_ASSERT_NOT_EQUAL(SCENE_ENTITY_NONE, entity);
    const capybara_take_t* take = scenario->take;
    if (take != NULL) {
        send(CAPYBARA_VIEW_COMMAND, take->view);
        TEST_ASSERT_TRUE(scene_animator_seek(scene, entity, SCENE_ANIMATOR_CURRENT, 0));
        send(CAPYBARA_CLIP_COMMAND, take->blend_ms);
    }
    TEST_ASSERT_TRUE(scene_animator_seek(scene, entity, SCENE_ANIMATOR_CURRENT, 0));
    const int clip = scene->animators[entity].layer.current.clip;
    const uint32_t duration_ms = take != NULL && take->blend_ms > 0
                                     ? (uint32_t)take->blend_ms
                                     : scene_animator_clip_duration_ms(scene, entity, clip);
    TEST_ASSERT_GREATER_THAN_UINT32(0, duration_ms);
    return (scenario_path_t){scene, entity, duration_ms};
}

/* The played profile against its budget, in a frame of its own for the same reason. */
static __attribute__((noinline)) void
judge_profile(const frame_profile_t* p) {
    r3d_perf_budget(scenario_row(), &r3d_perf_quarters[quarter_index], p->frame_us.avg, p->frame_us.p95,
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
        ESP_LOGI(TAG, "%s" R3D_PERF_QUARTER_FORMAT, line, r3d_perf_quarters[quarter_index].name);
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
    step_frame_t* records = memory_alloc(sizeof(*records) * (size_t)frames, MEMORY_PSRAM);
    TEST_ASSERT_NOT_NULL(records);
    /* One step and no recovery: the stepped policy holds it and times it. */
    const resolution_step_t size = render_lab_ladder[ladder_step];
    const resolution_config_t pinned = resolution_config(&size, 1, 1, RENDER_LAB_BUDGET_MAX_MS * MATH_US_PER_MS);
    render_context_set_dynamic_resolution(render_context_main(), &pinned, NULL, 0);
    (void)render_lab_test_frame_dt(0, false, NULL); /* warm-up, not sampled */
    for (int i = 0; i < frames; i++) {
        const int64_t start = timing_now_us();
        (void)render_lab_test_frame_dt(DYNRES_STEPS_FRAME_DT_MS, false, NULL);
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
        for (quarter_index = 0; quarter_index < R3D_PERF_QUARTER_COUNT; quarter_index++) {
            scenario = &profile_scenarios[i];
            RUN_TEST(test_real_time_frame_profile);
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
            RUN_TEST(test_dynres_steps);
        }
    }
#endif
}

SUITE_REGISTER_ON_REQUEST(run_render_lab_dynres_steps_suite);
