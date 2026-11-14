/*
 * Stress preset costs along each camera swing, paused in both quarters,
 * with fixed sizes for uniform presets. Stage goals only log distances;
 * each row's sum has a board ceiling. Verdicts are per preset and stage,
 * never summed over presets. Zero means unpegged (see the goal table).
 *
 * The stages are r3d's and the frame recorder's, which runs over a preset
 * that asks for it into a sink that keeps nothing: what composing the
 * records costs, without the link that carries them.
 */

#include "suites.h"
#ifdef DEVICE_BUILD
#include <limits.h>
#include <stdbool.h>
#include <string.h>
#include "apps/render_lab/render_lab.h"
#include "apps/render_lab/render_lab_dynres.h"
#include "apps/render_lab/render_lab_view.h"
#include "apps/render_lab/stress/scene_stress.h"
#include "apps/render_lab/tests/render_lab_test.h"
#include "core/tune.h"
#include "frame/test_frame.h"
#include "lab/test/lab_perf_quarters.h"
#include "render/context/render_context.h"
#include "render/record/frame_record.h"
#include "scene/animator/animator.h"
#include "testkit/cleanup.h"
#include "unity.h"

static const char* TAG = "stress_perf";

static lab_test_t saved;
static const lab_scene_t* preset;
static const lab_perf_quarter_t* quarter;
/* The row's own size, no ladder step: its goal rows' size name. */
#define STRESS_PERF_NO_STEP      (-1)
#define STRESS_PERF_DEFAULT_SIZE "default"

static int size_step;

/* r3d's stages, then the recorder's: a goal row's stage goals in this order. */
enum { STRESS_PERF_STAGES = R3D_STAGE_COUNT + 1 };

static const char* const stress_perf_stages[STRESS_PERF_STAGES] = {
    R3D_STAGE_CULL_NAME, R3D_STAGE_TRANSFORM_NAME, R3D_STAGE_DRAW_NAME, R3D_STAGE_UPSCALE_NAME, FRAME_RECORD_STAGE_NAME,
};
static int saved_filter;
static int saved_layout;

typedef struct {
    const char* key;
    const char* quarter;
    const char* size;
    int64_t stage_us[STRESS_PERF_STAGES];
    perf_budget_t sum;
} stress_goal_t;

/* Printed by tools/stress_goals.py from perf-board captures of this suite,
 * which owns the margins: regenerate the rows rather than editing them. The
 * sum's ceiling bounds the mean over a row's poses, as its goal does.
 * uniform-full stays unpegged (--unpegged): its full-view uniform fill varies
 * more between runs than any step a change makes. */
static const stress_goal_t stress_goals[] = {
    {"stress-open", "portrait", "default", {260, 800, 5450, 2250, 0}, {8700, 10010}},
    {"stress-open", "landscape", "default", {260, 780, 5650, 2240, 0}, {8870, 10210}},
    {"stress-stack", "portrait", "default", {220, 410, 8000, 2410, 0}, {10970, 12620}},
    {"stress-stack", "landscape", "default", {220, 420, 8060, 2430, 0}, {11070, 12740}},
    {"stress-tiny-copies", "portrait", "default", {490, 2570, 6570, 2050, 0}, {11600, 13340}},
    {"stress-tiny-copies", "landscape", "default", {490, 2550, 6770, 2040, 0}, {11770, 13540}},
    {"stress-big-tris", "portrait", "default", {140, 60, 2330, 2460, 0}, {4950, 5700}},
    {"stress-big-tris", "landscape", "default", {140, 60, 2230, 2470, 0}, {4860, 5590}},
    {"stress-per-frame", "portrait", "default", {200, 300, 6570, 2350, 0}, {9390, 10800}},
    {"stress-per-frame", "landscape", "default", {210, 320, 6930, 2320, 0}, {9740, 11210}},
    {"stress-two-sided", "portrait", "default", {290, 890, 12050, 2280, 0}, {15450, 17770}},
    {"stress-two-sided", "landscape", "default", {280, 890, 11940, 2300, 0}, {15350, 17660}},
    {"stress-noisy-tiny", "portrait", "default", {370, 1500, 11660, 2400, 0}, {15830, 18210}},
    {"stress-noisy-tiny", "landscape", "default", {370, 1490, 11530, 2400, 0}, {15730, 18090}},
    {"stress-mix", "portrait", "default", {620, 15460, 23580, 2050, 0}, {41660, 47910}},
    {"stress-mix", "landscape", "default", {640, 15280, 25440, 2060, 0}, {43280, 49780}},
    {"stress-static-copies", "portrait", "default", {460, 2060, 3720, 2350, 0}, {8510, 9790}},
    {"stress-static-copies", "landscape", "default", {460, 2060, 3830, 2310, 0}, {8610, 9910}},
    {"stress-uniform-mid", "portrait", "default", {140, 60, 1990, 2460, 0}, {4630, 5330}},
    {"stress-uniform-mid", "portrait", "368x448", {130, 60, 10210, 8920, 0}, {19290, 22190}},
    {"stress-uniform-mid", "portrait", "368x224", {130, 60, 4550, 22010, 0}, {26710, 30720}},
    {"stress-uniform-mid", "landscape", "default", {140, 60, 1920, 2470, 0}, {4550, 5240}},
    {"stress-uniform-mid", "landscape", "368x448", {130, 60, 9800, 8940, 0}, {18880, 21720}},
    {"stress-uniform-mid", "landscape", "368x224", {130, 60, 4350, 24380, 0}, {28880, 33220}},
    {"stress-uniform-full", "portrait", "default", {0, 0, 0, 0, 0}, {0, 0}},
    {"stress-uniform-full", "portrait", "368x448", {0, 0, 0, 0, 0}, {0, 0}},
    {"stress-uniform-full", "portrait", "368x224", {0, 0, 0, 0, 0}, {0, 0}},
    {"stress-uniform-full", "landscape", "default", {0, 0, 0, 0, 0}, {0, 0}},
    {"stress-uniform-full", "landscape", "368x448", {0, 0, 0, 0, 0}, {0, 0}},
    {"stress-uniform-full", "landscape", "368x224", {0, 0, 0, 0, 0}, {0, 0}},
    {"stress-card", "portrait", "default", {400, 3620, 14900, 2500, 0}, {21340, 24550}},
    {"stress-card", "landscape", "default", {390, 3000, 14770, 2540, 0}, {20600, 23690}},
    {"stress-culled", "portrait", "default", {170, 110, 2140, 2260, 0}, {4620, 5320}},
    {"stress-culled", "landscape", "default", {140, 90, 1930, 2260, 0}, {4370, 5030}},
    {"stress-near", "portrait", "default", {420, 1690, 10640, 2470, 0}, {15170, 17450}},
    {"stress-near", "landscape", "default", {410, 1580, 11130, 2440, 0}, {15490, 17820}},
    {"stress-few-copies", "portrait", "default", {260, 1090, 5290, 1990, 0}, {8600, 9890}},
    {"stress-few-copies", "landscape", "default", {260, 1080, 5720, 2050, 0}, {9080, 10450}},
    {"stress-record", "portrait", "default", {0, 0, 0, 0, 0}, {0, 0}},
    {"stress-record", "landscape", "default", {0, 0, 0, 0, 0}, {0, 0}},
};

static const char*
size_name(void) {
    return size_step == STRESS_PERF_NO_STEP ? STRESS_PERF_DEFAULT_SIZE : render_lab_test_step_name(size_step);
}

static const stress_goal_t*
stress_goal(void) {
    const char* size = size_name();
    for (size_t i = 0; i < sizeof stress_goals / sizeof stress_goals[0]; i++) {
        const stress_goal_t* goal = &stress_goals[i];
        if (strcmp(goal->key, preset->key) == 0 && strcmp(goal->quarter, quarter->name) == 0
            && strcmp(goal->size, size) == 0) {
            return goal;
        }
    }
    TEST_FAIL_MESSAGE("stress perf row has no goal entry");
    return NULL;
}

static void
report_goals(const stress_goal_t* goal, const int64_t sums[STRESS_PERF_STAGES], int poses) {
    char row[LAB_PERF_ROW_NAME_SIZE];
    const bool own_size = size_step == STRESS_PERF_NO_STEP;
    int length = snprintf(row, sizeof row, "%s %s%s%s", goal->key, goal->quarter, own_size ? "" : " ",
                          own_size ? "" : goal->size);
    TEST_ASSERT_TRUE(length > 0 && (size_t)length < sizeof row);
    int64_t total = 0;
    for (int i = 0; i < STRESS_PERF_STAGES; i++) {
        char stage[LAB_PERF_ROW_NAME_SIZE];
        length = snprintf(stage, sizeof stage, "%s %s", row, stress_perf_stages[i]);
        TEST_ASSERT_TRUE(length > 0 && (size_t)length < sizeof stage);
        const int64_t mean = sums[i] / poses;
        (void)perf_goal(stage, mean, goal->stage_us[i]);
        total += mean;
    }
    lab_perf_named_budget(row, total, total, &goal->sum, true);
}

/* The recorder's sink for a timed run: takes every frame and keeps nothing. */
static bool
discard_frame(void* sink, const uint8_t* bytes, size_t count) {
    (void)sink;
    (void)bytes;
    (void)count;
    return true;
}

static void
release_fixture(void) {
    frame_record_stop();
    render_lab_test_show_view(RENDER_DEBUG_VIEW_SHADED);
    render_context_set_dynamic_resolution(render_context_main(), NULL, NULL, 0);
    TEST_ASSERT_TRUE(tune_set(tune_shared(), TUNE_NAME(render_lab, upscale), saved_filter));
    lab_test_exit(&saved);
    TEST_ASSERT_TRUE(tune_set(tune_shared(), TUNE_NAME(render_lab, layout), saved_layout));
}

static void
test_stress_stages(void) {
    const stress_goal_t* goal = stress_goal();
    saved_filter = render_lab_upscale_selected();
    saved_layout = (int)lab_view_layout(&render_lab_view_tunables);
    const stress_params_t* params = preset->data;
    TEST_ASSERT_TRUE(tune_set(tune_shared(), TUNE_NAME(render_lab, layout), LAB_LAYOUT_FULL));
    lab_test_enter(&saved, &render_lab, preset->key);
    suite_set_test_cleanup(release_fixture);
    test_hold_quarter(quarter->quarter);
    (void)lab_test_frame_dt(0, false, NULL);
    if (size_step != STRESS_PERF_NO_STEP) {
        const resolution_step_t size = render_lab_ladder[size_step];
        const resolution_config_t one = resolution_config(&size, 1, 1, INT32_MAX);
        render_context_set_dynamic_resolution(render_context_main(), &one, NULL, 0);
        TEST_ASSERT_TRUE(tune_set(tune_shared(), TUNE_NAME(render_lab, upscale), UPSCALE_FILTER_SURFACE));
    }
    if (params->record) {
        static const frame_record_out_t discard = {discard_frame, NULL};
        TEST_ASSERT_TRUE_MESSAGE(frame_record_start(&discard, FRAME_RECORD_KEYFRAME_EVERY),
                                 "the recorder did not start");
    }
    const lab_path_t path = preset->path();
    TEST_ASSERT_FALSE_MESSAGE(handle_is_none(path.scene), "the stress scene did not load");
    const entity_t camera = path.entity;
    TEST_ASSERT_FALSE(handle_is_none(camera));
    TEST_ASSERT_TRUE(animator_seek(camera, ANIMATOR_CURRENT, 0));
    const uint32_t duration_ms = animator_clip_duration_ms(camera, animator_current_clip(camera));
    TEST_ASSERT_GREATER_THAN_UINT32(0, duration_ms);
    int64_t sums[STRESS_PERF_STAGES] = {0};
    for (int i = 0; i < params->perf_poses; i++) {
        int64_t stage_us[STRESS_PERF_STAGES];
        render_lab_test_replay_pose(TAG, camera, duration_ms, (int32_t)((int64_t)duration_ms * i / params->perf_poses),
                                    preset->key, quarter->name, stress_perf_stages, STRESS_PERF_STAGES, stage_us);
        for (int stage = 0; stage < STRESS_PERF_STAGES; stage++) {
            sums[stage] += stage_us[stage];
        }
    }
    report_goals(goal, sums, params->perf_poses);
    release_fixture();
    suite_clear_test_cleanup();
}
#endif

void
run_stress_perf_suite(void) {
#ifdef DEVICE_BUILD
    for (int i = 0; i < STRESS_PRESET_COUNT; i++) {
        preset = &scene_stress[i];
        const stress_params_t* params = preset->data;
        const int sizes[] = {STRESS_PERF_NO_STEP, RENDER_LAB_STEP_FULL, RENDER_LAB_STEP_HALF_HEIGHT};
        const int size_count = params->surface == STRESS_SURFACE_UNIFORM ? (int)(sizeof sizes / sizeof sizes[0]) : 1;
        for (int q = 0; q < (int)LAB_PERF_QUARTER_COUNT; q++) {
            quarter = &lab_perf_quarters[q];
            for (int s = 0; s < size_count; s++) {
                size_step = sizes[s];
                RUN_TEST_CASE(test_stress_stages, preset->key, quarter->name, size_name());
            }
        }
    }
#endif
}

SUITE_REGISTER(run_stress_perf_suite);
