/* On request: what a generated frame costs on the board, for the frame
 * generation experiment (docs/render/Frame-Generation.md). Each scenario's
 * path is drawn at every half-picture ladder step; at poses along it the
 * drawn raster is forward-warped one display step on, into a second cleared
 * colour and depth target, and its holes are filled from the drawn frame.
 * The present is the frame's own half-picture expand. Removable as this file. */
#include "suites.h"
#ifdef DEVICE_BUILD
#include <string.h>

#include "apps/render_lab/baked_families.h"
#include "apps/render_lab/render_lab_dynres.h"
#include "apps/render_lab/render_lab_scene.h"
#include "apps/render_lab/render_lab_view.h"
#include "apps/render_lab/scene_baked.h"
#include "apps/render_lab/sponza_content.h"
#include "apps/render_lab/tests/r3d_perf_quarters.h"
#include "apps/render_lab/tests/render_lab_test.h"
#include "core/job.h"
#include "core/memory.h"
#include "core/timing.h"
#include "esp_log.h"
#include "gfx/gfx.h"
#include "math/scalar/mathf.h"
#include "render/context/render_context.h"
#include "render/r3d_span.h"
#include "render/raster_internal.h"
#include "scene/camera/camera.h"
#include "scene/scene_internal.h"
#include "testkit/cleanup.h"
#include "unity.h"

static const char* TAG = "framegen";

/* How far ahead a generated frame looks: one displayed frame. */
#define FRAMEGEN_AHEAD_MS    RENDER_LAB_FRAME_TARGET_MS
/* Poses timed along each path. */
#define FRAMEGEN_POSES       24
#define FRAMEGEN_DEPTH_SCALE (1.0F / (float)R3D_DEPTH_NEAREST)
/* A warped sky pixel: farther than anything drawn, but not a hole (0). */
#define FRAMEGEN_DEPTH_SKY   1U
#define FRAMEGEN_PERCENT     100
#define FRAMEGEN_LINE                                                                                                  \
    "framegen: %s %dx%d poses=%d frame_us=%d present_us=%d warp1_us=%d/%d warp2_us=%d/%d fill_us=%d holes_pct=%d.%02d"

typedef struct {
    const render_lab_scene_t* scene;
    const char* pack;
    const char* camera;
} framegen_scenario_t;

static const framegen_scenario_t scenarios[] = {
    {&scene_bistro[SCENE_BAKED_FITTED_FULL], BISTRO_SCENE, BISTRO_CAMERA},
    {&scene_sponza[SCENE_BAKED_FITTED_FULL], SPONZA_SCENE, NULL},
};

/* One core's share of a warp: source rows [first, end) of the drawn raster
 * into the whole target. */
typedef struct {
    const uint16_t* colour;
    const uint16_t* depth;
    uint16_t* out_colour;
    uint16_t* out_depth;
    int width, height, first, end;
    const mat4f_t* map;
    projectf_t project;
} warp_rows_t;

/* One lens-space axis of the map along a row, the row's v folded in:
 * the axis is du u + base + dw w. */
typedef struct {
    float du, base, dw;
} lens_axis_t;

static inline float
axis_at(lens_axis_t a, float u, float w) {
    return (a.du * u) + a.base + (a.dw * w);
}

/* The warped depth word: the sky stays the farthest, anything drawn sits above it. */
static inline uint32_t
moved_depth(uint16_t depth, float moved) {
    if (depth == R3D_DEPTH_EMPTY) {
        return FRAMEGEN_DEPTH_SKY;
    }
    const float nearest = (float)R3D_DEPTH_NEAREST;
    const float above_sky = (float)(FRAMEGEN_DEPTH_SKY + 1U);
    return (uint32_t)(moved >= nearest ? nearest : moved < above_sky ? above_sky : moved);
}

/* One source pixel lands at the nearest target pixel; the nearer point wins.
 * Lens space is pixels at unit depth: depth word d is the point (u, v, 1) near_z / iz,
 * and through the map its new inverse depth is d / z. The sky (d = 0) has w = 0,
 * so it only turns. to_w turns a depth word into w. */
static inline void
splat(const warp_rows_t* job, const lens_axis_t axes[3], float to_w, float u, uint16_t depth, uint16_t colour) {
    const float d = (float)depth;
    const float w = d * to_w;
    const float z = axis_at(axes[2], u, w);
    if (!(z > 0.0F)) {
        return;
    }
    const float inv = mathf_recip(z);
    const float px = (axis_at(axes[0], u, w) * inv) + job->project.cx;
    const float py = (axis_at(axes[1], u, w) * inv) + job->project.cy;
    if (!(px >= 0.0F && py >= 0.0F && px < (float)job->width && py < (float)job->height)) {
        return;
    }
    const size_t at = ((size_t)py * (size_t)job->width) + (size_t)px;
    const uint32_t nd = moved_depth(depth, d * inv);
    if (nd > job->out_depth[at]) {
        job->out_depth[at] = (uint16_t)nd;
        job->out_colour[at] = colour;
    }
}

static void
warp_rows(void* ctx) {
    const warp_rows_t* job = ctx;
    const float to_w = FRAMEGEN_DEPTH_SCALE / job->project.near_z;
    const float (*m)[4] = job->map->m;
    for (int y = job->first; y < job->end; y++) {
        const float v = (float)y + 0.5F - job->project.cy;
        const lens_axis_t axes[3] = {
            {m[0][0], (m[0][1] * v) + m[0][2], m[0][3]},
            {m[1][0], (m[1][1] * v) + m[1][2], m[1][3]},
            {m[2][0], (m[2][1] * v) + m[2][2], m[2][3]},
        };
        const size_t row = (size_t)y * (size_t)job->width;
        float u = 0.5F - job->project.cx;
        for (int x = 0; x < job->width; x++, u += 1.0F) {
            splat(job, axes, to_w, u, job->depth[row + (size_t)x], job->colour[row + (size_t)x]);
        }
    }
}

/* The warp, the target cleared first, on one core or split by source rows
 * between both (two rows may race for one target pixel; the timing does not care). */
static int64_t
warp(warp_rows_t job, bool both_cores) {
    const size_t pixels = (size_t)job.width * (size_t)job.height;
    const int64_t start = timing_now_us();
    memset(job.out_depth, 0, pixels * sizeof(uint16_t));
    if (both_cores) {
        warp_rows_t second = job;
        job.end = job.height / 2;
        second.first = job.end;
        job_run_split(warp_rows, &job, &second, sizeof second);
    } else {
        warp_rows(&job);
    }
    return timing_now_us() - start;
}

/* Holes take the drawn frame's colour at the same pixel; returns the hole count. */
static int
fill_previous(const warp_rows_t* job) {
    const size_t pixels = (size_t)job->width * (size_t)job->height;
    int holes = 0;
    for (size_t i = 0; i < pixels; i++) {
        if (job->out_depth[i] == 0) {
            job->out_colour[i] = job->colour[i];
            holes++;
        }
    }
    return holes;
}

static render_lab_test_t saved;
static const framegen_scenario_t* scenario;
static int ladder_step;
static uint16_t* target_colour;
static uint16_t* target_depth;

static void
release_fixture(void) {
    memory_free(target_colour);
    memory_free(target_depth);
    target_colour = NULL;
    target_depth = NULL;
    render_lab_test_exit(&saved);
}

/* The view through the camera at `t_ms` along its path, as the scene draws it. */
static render_view_t
view_at(scene_t* scene, scene_entity_t camera, const camera_t* lens, uint32_t t_ms) {
    TEST_ASSERT_TRUE(scene_animator_seek(scene, camera, SCENE_ANIMATOR_CURRENT, t_ms));
    const transformf_t pose = scene_entity_world(scene, camera);
    return camera_view(lens, &pose, (viewport_t){GFX_WIDTH, GFX_HEIGHT, r3d_perf_quarters[0].quarter});
}

typedef struct {
    int64_t frame, present, warp1, warp1_max, warp2, warp2_max, fill, holes, pixels;
} totals_t;

/* The test's sums; static, so the deep frame draw below has the stack to itself. */
static totals_t sum;

/* The frame drawn at `t_ms` warped one display step on, timed. Not inlined: its
 * views, lenses and map are off the stack while the frame draws. */
static __attribute__((noinline)) void
time_warp(scene_t* scene, scene_entity_t camera, const camera_t* lens, uint32_t t_ms) {
    const raster_t* raster = &render_context_main()->raster;
    const render_view_t now = view_at(scene, camera, lens, t_ms);
    const render_view_t next = view_at(scene, camera, lens, t_ms + FRAMEGEN_AHEAD_MS);
    r3d_lens_t before;
    r3d_lens_t after;
    raster_lens(raster, &now, 1, &before);
    raster_lens(raster, &next, 1, &after);
    const mat4f_t map = mat4f_mul_affine(after.m, mat4f_invert_affine(before.m));
    const warp_rows_t job = {raster_color(raster),
                             raster_depth(raster),
                             target_colour,
                             target_depth,
                             raster->width,
                             raster->height,
                             0,
                             raster->height,
                             &map,
                             before.project};
    const int64_t one = warp(job, false);
    const int64_t two = warp(job, true);
    const int64_t fill_start = timing_now_us();
    sum.holes += fill_previous(&job);
    sum.fill += timing_now_us() - fill_start;
    sum.warp1 += one;
    sum.warp1_max = one > sum.warp1_max ? one : sum.warp1_max;
    sum.warp2 += two;
    sum.warp2_max = two > sum.warp2_max ? two : sum.warp2_max;
    sum.pixels += (int64_t)raster->width * raster->height;
}

static void
test_generated_frame_cost(void) {
    render_lab_test_enter(&saved, scenario->scene->key);
    suite_set_test_cleanup(release_fixture);
    render_lab_test_hold_quarter(r3d_perf_quarters[0].quarter);
    scene_t* scene = render_lab_test_loaded_scene(scenario->pack);
    TEST_ASSERT_NOT_NULL(scene);
    const char* camera_name = scenario->camera == NULL ? sponza_cameras[0] : scenario->camera;
    const scene_entity_t camera = scene_find(scene, name_hash(camera_name));
    TEST_ASSERT_NOT_EQUAL(SCENE_ENTITY_NONE, camera);
    const camera_t* lens = scene_camera_lens(scene, name_hash(camera_name));
    TEST_ASSERT_NOT_NULL(lens);
    TEST_ASSERT_TRUE(scene_animator_seek(scene, camera, SCENE_ANIMATOR_CURRENT, 0));
    const uint32_t duration_ms =
        scene_animator_clip_duration_ms(scene, camera, scene->animators[camera].layer.current.clip);
    TEST_ASSERT_GREATER_THAN_UINT32(FRAMEGEN_AHEAD_MS, duration_ms);

    const resolution_step_t size = render_lab_ladder[ladder_step];
    const resolution_config_t pinned = resolution_config(&size, 1, 1, RENDER_LAB_BUDGET_MAX_MS * MATH_US_PER_MS);
    render_context_set_dynamic_resolution(render_context_main(), &pinned, NULL, 0);
    const size_t pixels = (size_t)size.width * (size_t)size.height;
    target_colour = memory_alloc(pixels * sizeof(uint16_t), MEMORY_PSRAM);
    target_depth = memory_alloc(pixels * sizeof(uint16_t), MEMORY_PSRAM);
    TEST_ASSERT_NOT_NULL(target_colour);
    TEST_ASSERT_NOT_NULL(target_depth);
    (void)render_lab_test_frame_dt(0, false, NULL); /* warm-up, not sampled */

    sum = (totals_t){0};
    for (int i = 0; i < FRAMEGEN_POSES; i++) {
        const uint32_t t_ms = (uint32_t)(((uint64_t)(duration_ms - FRAMEGEN_AHEAD_MS) * (uint64_t)i) / FRAMEGEN_POSES);
        TEST_ASSERT_TRUE(scene_animator_seek(scene, camera, SCENE_ANIMATOR_CURRENT, t_ms));
        const int64_t start = timing_now_us();
        (void)render_lab_test_frame_dt(0, false, NULL);
        sum.frame += timing_now_us() - start;
        sum.present += render_context_frame(render_context_main()).upscale_us;
        TEST_ASSERT_EQUAL_INT(size.width, render_context_main()->raster.width);
        TEST_ASSERT_EQUAL_INT(size.height, render_context_main()->raster.height);
        time_warp(scene, camera, lens, t_ms);
    }
    const int64_t hundredths = (sum.holes * FRAMEGEN_PERCENT * FRAMEGEN_PERCENT) / sum.pixels;
    ESP_LOGI(TAG, FRAMEGEN_LINE, scenario->scene->key, size.width, size.height, FRAMEGEN_POSES,
             (int)(sum.frame / FRAMEGEN_POSES), (int)(sum.present / FRAMEGEN_POSES), (int)(sum.warp1 / FRAMEGEN_POSES),
             (int)sum.warp1_max, (int)(sum.warp2 / FRAMEGEN_POSES), (int)sum.warp2_max,
             (int)(sum.fill / FRAMEGEN_POSES), (int)(hundredths / FRAMEGEN_PERCENT),
             (int)(hundredths % FRAMEGEN_PERCENT));
    release_fixture();
    suite_clear_test_cleanup();
}
#endif

/* Every scenario at every half-picture ladder step, portrait. */
void
run_framegen_perf_suite(void) {
#ifdef DEVICE_BUILD
    for (size_t i = 0; i < sizeof scenarios / sizeof scenarios[0]; i++) {
        scenario = &scenarios[i];
        for (ladder_step = 0; ladder_step < RENDER_LAB_LADDER_STEPS; ladder_step++) {
            if (render_lab_ladder[ladder_step].width * 2 == GFX_WIDTH) {
                RUN_TEST(test_generated_frame_cost);
            }
        }
    }
#endif
}

SUITE_REGISTER_ON_REQUEST(run_framegen_perf_suite);
