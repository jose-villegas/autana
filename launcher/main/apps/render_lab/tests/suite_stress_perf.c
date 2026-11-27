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
#include "frame_profile.h"
#include "lab/test/lab_perf_quarters.h"
#include "render/context/render_context.h"
#include "render/record/frame_record.h"
#include "scene/animator/animator.h"
#include "scene/scene_anim_stages.h"
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

/* Goal rows store raster stages, the recorder and, for skinned presets, animation stages. */
enum {
    STRESS_PERF_STATIC_STAGES = R3D_STAGE_COUNT + 1,
    STRESS_PERF_ANIMATION_STAGES = 3,
    STRESS_PERF_STAGES = STRESS_PERF_STATIC_STAGES + STRESS_PERF_ANIMATION_STAGES,
    STRESS_PERF_SAMPLED_STAGES = STRESS_PERF_STAGES + 1
};

static const char* const stress_perf_stages[STRESS_PERF_STAGES] = {
    R3D_STAGE_CULL_NAME,     R3D_STAGE_TRANSFORM_NAME,   R3D_STAGE_DRAW_NAME,         R3D_STAGE_UPSCALE_NAME,
    FRAME_RECORD_STAGE_NAME, SCENE_ANIM_STAGE_POSE_NAME, SCENE_ANIM_STAGE_LIGHT_NAME, SCENE_ANIM_STAGE_SKIN_NAME,
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
    {"stress-carved-near", "portrait", "default", {0, 0, 0, 0, 0}, {0, 0}},
    {"stress-carved-near", "landscape", "default", {0, 0, 0, 0, 0}, {0, 0}},
    {"stress-carved-far", "portrait", "default", {0, 0, 0, 0, 0}, {0, 0}},
    {"stress-carved-far", "landscape", "default", {0, 0, 0, 0, 0}, {0, 0}},
    {"stress-textured", "portrait", "default", {0, 0, 0, 0, 0}, {0, 0}},
    {"stress-textured", "landscape", "default", {0, 0, 0, 0, 0}, {0, 0}},
    {"stress-textured-far", "portrait", "default", {0, 0, 0, 0, 0}, {0, 0}},
    {"stress-textured-far", "landscape", "default", {0, 0, 0, 0, 0}, {0, 0}},
    {"stress-foliage-near", "portrait", "default", {200, 200, 24720, 2430, 0}, {27520, 31650}},
    {"stress-foliage-near", "landscape", "default", {200, 190, 22430, 2410, 0}, {25200, 28980}},
    {"stress-foliage-near-opaque", "portrait", "default", {190, 220, 31020, 2420, 0}, {33800, 38870}},
    {"stress-foliage-near-opaque", "landscape", "default", {180, 200, 28310, 2390, 0}, {31040, 35700}},
    {"stress-foliage-far", "portrait", "default", {340, 1800, 18370, 2250, 0}, {22710, 26120}},
    {"stress-foliage-far", "landscape", "default", {350, 1790, 15260, 2250, 0}, {19610, 22560}},
    {"stress-foliage-far-opaque", "portrait", "default", {340, 1800, 18230, 2260, 0}, {22610, 26010}},
    {"stress-foliage-far-opaque", "landscape", "default", {340, 1790, 15130, 2260, 0}, {19510, 22440}},
    /* Butterfly goals cover flapping wings across layout seeds; stress.flap stays unpegged. */
    {"stress-butterflies", "portrait", "default", {580, 2510, 6490, 2200, 10}, {11740, 13510}},
    {"stress-butterflies", "landscape", "default", {570, 2500, 6860, 2230, 10}, {12100, 13920}},
    {"stress-few-butterflies", "portrait", "default", {190, 290, 1950, 2250, 10}, {4640, 5340}},
    {"stress-few-butterflies", "landscape", "default", {200, 290, 2040, 2270, 10}, {4760, 5480}},
    {"stress-record", "portrait", "default", {290, 1070, 9790, 2420, 1330}, {14810, 17040}},
    {"stress-record", "landscape", "default", {290, 1070, 10620, 2420, 1320}, {15650, 18000}},
    {"stress-cull-setup", "portrait", "default", {150, 60, 1000, 2250, 0}, {3420, 3940}},
    {"stress-cull-setup", "landscape", "default", {140, 70, 980, 2250, 0}, {3410, 3930}},
    {"stress-baked-tree", "portrait", "default", {370, 580, 6090, 2200, 10}, {9150, 10530}},
    {"stress-baked-tree", "landscape", "default", {390, 650, 7230, 2310, 10}, {10550, 12140}},
    {"stress-skinned-sh-blobs", "portrait", "default", {310, 1230, 7090, 2040, 10, 1530, 250, 2900}, {15260, 17550}},
    {"stress-skinned-sh-blobs", "landscape", "default", {310, 1240, 7810, 2130, 10, 1530, 250, 2890}, {16060, 18470}},
    {"stress-skinned-blob-warped", "portrait", "default", {310, 1230, 7220, 2030, 10, 1550, 250, 2900}, {15400, 17710}},
    {"stress-skinned-blob-warped",
     "landscape",
     "default",
     {310, 1210, 8080, 2140, 10, 1540, 250, 2920},
     {16370, 18830}},
    {"stress-contact-blobs", "portrait", "default", {190, 720, 4160, 2180, 10}, {7190, 8270}},
    {"stress-contact-blobs", "landscape", "default", {200, 700, 7320, 2210, 10}, {10320, 11870}},
    {"stress-skinned-feet-blobs", "portrait", "default", {320, 1240, 7680, 2020, 10, 1540, 250, 2880}, {15810, 18190}},
    {"stress-skinned-feet-blobs", "landscape", "default", {310, 1230, 8940, 2110, 10, 1530, 250, 2900}, {17160, 19740}},
    {"stress-skinned-silhouette-warped",
     "portrait",
     "default",
     {310, 1230, 8250, 2090, 10, 1560, 250, 2900},
     {16480, 18960}},
    {"stress-skinned-silhouette-warped",
     "landscape",
     "default",
     {310, 1240, 9170, 2150, 10, 1550, 260, 2900},
     {17490, 20120}},
    {"stress-skinned-silhouette-blobs",
     "portrait",
     "default",
     {310, 1240, 7670, 2100, 10, 1550, 250, 2910},
     {15900, 18290}},
    {"stress-skinned-silhouette-blobs",
     "landscape",
     "default",
     {320, 1230, 8460, 2170, 10, 1550, 260, 2890},
     {16810, 19340}},
    {"stress-small-grain", "portrait", "default", {0, 0, 0, 0, 0}, {0, 0}},
    {"stress-small-grain", "landscape", "default", {0, 0, 0, 0, 0}, {0, 0}},
    {"stress-small-dither", "portrait", "default", {0, 0, 0, 0, 0}, {0, 0}},
    {"stress-small-dither", "landscape", "default", {0, 0, 0, 0, 0}, {0, 0}},
    {"stress-small-off", "portrait", "default", {0, 0, 0, 0, 0}, {0, 0}},
    {"stress-small-off", "landscape", "default", {0, 0, 0, 0, 0}, {0, 0}},
    {"stress-stack-skip", "portrait", "default", {0, 0, 0, 0, 0}, {0, 0}},
    {"stress-stack-skip", "landscape", "default", {0, 0, 0, 0, 0}, {0, 0}},
    {"stress-settle", "portrait", "default", {0, 0, 0, 0, 0}, {0, 0}},
    {"stress-settle", "landscape", "default", {0, 0, 0, 0, 0}, {0, 0}},
    {"stress-carved-blend", "portrait", "default", {0, 0, 0, 0, 0}, {0, 0}},
    {"stress-carved-blend", "landscape", "default", {0, 0, 0, 0, 0}, {0, 0}},
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

static __attribute__((noinline)) void
report_goals(const stress_goal_t* goal, const int64_t sums[STRESS_PERF_SAMPLED_STAGES], int poses, int stages) {
    char row[LAB_PERF_ROW_NAME_SIZE];
    const bool own_size = strcmp(goal->size, STRESS_PERF_DEFAULT_SIZE) == 0;
    int length = snprintf(row, sizeof row, "%s %s%s%s", goal->key, goal->quarter, own_size ? "" : " ",
                          own_size ? "" : goal->size);
    TEST_ASSERT_TRUE(length > 0 && (size_t)length < sizeof row);
    int64_t total = 0;
    for (int i = 0; i < stages; i++) {
        char stage[LAB_PERF_ROW_NAME_SIZE];
        length = snprintf(stage, sizeof stage, "%s %s", row, stress_perf_stages[i]);
        TEST_ASSERT_TRUE(length > 0 && (size_t)length < sizeof stage);
        const int64_t mean = sums[i] / poses;
        (void)perf_goal(stage, mean, goal->stage_us[i]);
        total += mean;
    }
    lab_perf_named_budget(row, total, total, &goal->sum, true);
    if (((const stress_params_t*)preset->data)->surface == STRESS_SURFACE_HINGED) {
        char stage[LAB_PERF_ROW_NAME_SIZE];
        length = snprintf(stage, sizeof stage, "%s %s", row, STRESS_FLAP_STAGE_NAME);
        TEST_ASSERT_TRUE(length > 0 && (size_t)length < sizeof stage);
        (void)perf_goal(stage, sums[STRESS_PERF_STAGES] / poses, 0);
    }
}

/* The recorder's sink for a timed run: takes every frame and keeps nothing. */
static bool
discard_frame(void* sink, const uint8_t* bytes, size_t count) {
    (void)sink;
    (void)bytes;
    (void)count;
    return true;
}

static struct {
    const char* sampled[STRESS_PERF_SAMPLED_STAGES];
    int64_t sums[STRESS_PERF_SAMPLED_STAGES];
    int64_t stage_us[STRESS_PERF_SAMPLED_STAGES];
}* samples;

static void
release_fixture(void) {
    memory_free(samples);
    samples = NULL;
    frame_record_stop();
    render_lab_test_show_view(RENDER_DEBUG_VIEW_SHADED);
    render_context_set_dynamic_resolution(render_context_main(), NULL, NULL, 0);
    TEST_ASSERT_TRUE(tune_set(tune_shared(), TUNE_NAME(render_lab, upscale), saved_filter));
    lab_test_exit(&saved);
    TEST_ASSERT_TRUE(tune_set(tune_shared(), TUNE_NAME(render_lab, layout), saved_layout));
}

static __attribute__((noinline)) entity_t
stress_camera(void) {
    const lab_path_t path = preset->path();
    TEST_ASSERT_FALSE_MESSAGE(handle_is_none(path.scene), "the stress scene did not load");
    return path.entity;
}

static __attribute__((noinline)) void
configure_stress(const stress_params_t* params) {
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
    samples = memory_alloc(sizeof *samples, MEMORY_APP);
    TEST_ASSERT_NOT_NULL(samples);
    memset(samples, 0, sizeof *samples);
    test_hold_quarter(quarter->quarter);
    (void)lab_test_frame_dt(0, false, NULL);
    configure_stress(params);
    const entity_t camera = stress_camera();
    TEST_ASSERT_FALSE(handle_is_none(camera));
    TEST_ASSERT_TRUE(animator_seek(camera, ANIMATOR_CURRENT, 0));
    const uint32_t duration_ms = animator_clip_duration_ms(camera, animator_current_clip(camera));
    TEST_ASSERT_GREATER_THAN_UINT32(0, duration_ms);
    const bool animated = stress_skinned(params);
    const int stages = animated ? STRESS_PERF_STAGES : STRESS_PERF_STATIC_STAGES;
    const uint32_t dt_ms = animated ? LAB_TEST_FRAME_MS : 0;
    memcpy(samples->sampled, stress_perf_stages, sizeof stress_perf_stages);
    samples->sampled[STRESS_PERF_STAGES] = STRESS_FLAP_STAGE_NAME;
    const render_settings_t* settings = render_context_settings(render_context_main());
    const int draws =
        params->hold_camera ? settings->settle.hold_frames + RENDER_SETTLE_BANDS : FRAME_PROFILE_REPLAY_DRAWS;
    for (int i = 0; i < params->perf_poses; i++) {
        render_lab_test_replay_pose(TAG, camera, duration_ms, (int32_t)stress_pose_ms(params, duration_ms, i),
                                    preset->key, quarter->name, samples->sampled, STRESS_PERF_SAMPLED_STAGES,
                                    samples->stage_us, dt_ms, draws);
        for (int stage = 0; stage < STRESS_PERF_SAMPLED_STAGES; stage++) {
            samples->sums[stage] += samples->stage_us[stage];
        }
    }
    report_goals(goal, samples->sums, params->perf_poses, stages);
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
