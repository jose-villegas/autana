/*
 * Device-only suite: where a raster frame's time goes at each render size,
 * along the render lab's flythrough. One line per size gives the raster's
 * frame_cost stages on both cores and one core's span stages, which is what
 * a dynamic-resolution controller's steps and cost model are sized from.
 * Then each policy flies the path through the scene manager at a fixed
 * frame step, one record per frame, against two budgets.
 * launcher/tools/r3d/dynres_report.py reads the capture.
 *
 * Runs under DEVICE_BUILD only - needs PSRAM, core 1 and a clock.
 */
#include "suites.h" /* portable - needed by SUITE_REGISTER() even on host */

#ifdef DEVICE_BUILD

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "unity.h"

#include "esp_log.h"

#include "apps/render_lab/sponza_content.h"
#include "asset/asset_store.h"
#include "gfx/gfx.h"
#include "render/context/render_context.h"
#include "render/r3d.h"
#include "render/r3d_pipeline.h"
#include "render/r3d_span_internal.h"
#include "render/resolution/resolution.h"
#include "scene/scene.h"
#include "scene/scene_shell.h"
#include "util/runtime/frame_cost.h"
#include "util/runtime/memory.h"
#include "util/runtime/timing.h"

static const char* TAG = "scale_perf";

/* Denser than the scene's own pose spacing, so a size's mean is not two poses. */
#define POSE_EVERY_MS 2500
#define POSES_MAX     64

/* Width-first and height-first ladders keep every upscale on a fast path,
 * with the last step reserved for recovery. */
static const resolution_step_t width_steps[] = {
    {368, 448}, {184, 448}, {184, 358}, {184, 298}, {184, 224}, {184, 179}, {184, 149},
};

typedef struct {
    const char* name;
    const resolution_step_t* steps;
    int count, recovery_from, half;
} ladder_t;

static const ladder_t ladders[] = {
    {"width", width_steps, 7, 6, 4},
    {"height", sponza_ladder, SPONZA_LADDER_STEPS, SPONZA_LADDER_RECOVERY, SPONZA_LADDER_HALF},
};
#define LADDER_COUNT ((int)(sizeof ladders / sizeof ladders[0]))

/* The stage split measures every ladder step, then these: sizes off the
 * fast upscale widths, among them the isotropic 2.5x and 3x the floor and
 * the recovery step are costed against. */
static const resolution_step_t mapped_sizes[] = {
    {294, 358}, {276, 336}, {245, 298}, {210, 256}, {245, 224}, {147, 179}, {122, 149},
};

static scene_t* scene;
static const asset_pack_t* pack;
static const r3d_scene_camera_t* path;
static r3d_lit_mesh_t mesh;

static int
compare_us(const void* a, const void* b) {
    const int32_t x = *(const int32_t*)a;
    const int32_t y = *(const int32_t*)b;
    return (x > y) - (x < y);
}

/* One core's whole draw, cleared first, stopped after the stage `stop`
 * names (r3d_span_stop_after; 0 runs every stage). */
static int64_t
one_core_draw_us(const raster_t* raster, const r3d_pipeline_buffers_t* b, const r3d_lens_t* lens, int visible,
                 int stop) {
    const size_t pixels = (size_t)raster->width * (size_t)raster->height;
    memset(gfx_render_target_depth(&b->picture, 0), 0, pixels * sizeof(uint16_t));
    r3d_span_stop_after = stop;
    const int64_t start = timing_now_us();
    const r3d_span_target_t target = {b->picture, NULL, 0};
    r3d_pipeline_draw(raster->instances[0].mesh, lens, b->culled + 1, visible, b->cs, b->rows, &target);
    const int64_t us = timing_now_us() - start;
    r3d_span_stop_after = 0;
    return us;
}

typedef struct {
    int64_t setup, rows, span_setup, fill, clear;
} span_split_t;

/* One core, one pose: each stage as the difference of two stopped draws;
 * not inlined, so its frame is gone before the raster draws. */
static __attribute__((noinline)) void
add_span_split(raster_t* raster, uint32_t t_ms, span_split_t* sum) {
    const camera_t camera = r3d_scene_camera_at(path, t_ms);
    r3d_lens_t lens;
    r3d_lens_init(&lens, &camera, mesh.position_scale, (viewport_t){GFX_WIDTH, GFX_HEIGHT, 0});
    r3d_lens_fit(&lens, raster->width, raster->height);
    const r3d_pipeline_buffers_t b = r3d_pipeline_carve(raster);
    const int visible = r3d_pipeline_cull(&mesh, &lens, b.culled + 1);
    r3d_pipeline_transform(&mesh, &lens, b.culled + 1, visible, b.cs, b.rows);
    const size_t pixels = (size_t)raster->width * (size_t)raster->height;
    const int64_t clear_from = timing_now_us();
    memset(gfx_render_target_depth(&b.picture, 0), 0, pixels * sizeof(uint16_t));
    sum->clear += timing_now_us() - clear_from;
    const int64_t setup = one_core_draw_us(raster, &b, &lens, visible, 1);
    const int64_t rows = one_core_draw_us(raster, &b, &lens, visible, 2);
    const int64_t span_setup = one_core_draw_us(raster, &b, &lens, visible, 3);
    const int64_t whole = one_core_draw_us(raster, &b, &lens, visible, 0);
    sum->setup += setup;
    sum->rows += rows - setup;
    sum->span_setup += span_setup - rows;
    sum->fill += whole - span_setup;
}

static void
measure_size(raster_t* raster, resolution_step_t size, int32_t* frame_us, char* report) {
    raster->width = size.width;
    raster->height = size.height;
    (void)frame_cost_take_report(1, report, FRAME_COST_REPORT_MAX); /* forget whatever ran before */
    const uint32_t period = r3d_scene_camera_period_ms(path);
    int poses = 0;
    int64_t triangles = 0;
    for (uint32_t t_ms = 0; t_ms < period && poses < POSES_MAX; t_ms += POSE_EVERY_MS) {
        const camera_t camera = r3d_scene_camera_at(path, t_ms);
        const int64_t start = timing_now_us();
        const raster_stats_t stats = raster_draw(raster, &camera, 0);
        raster_upscale(raster);
        frame_us[poses++] = (int32_t)(timing_now_us() - start);
        triangles += stats.triangles;
    }
    (void)frame_cost_take_report((uint32_t)poses, report, FRAME_COST_REPORT_MAX);

    span_split_t spans = {0};
    for (uint32_t t_ms = 0; t_ms < period && t_ms / POSE_EVERY_MS < POSES_MAX; t_ms += POSE_EVERY_MS) {
        add_span_split(raster, t_ms, &spans);
    }

    int64_t sum = 0;
    for (int i = 0; i < poses; i++) {
        sum += frame_us[i];
    }
    qsort(frame_us, (size_t)poses, sizeof *frame_us, compare_us);
    ESP_LOGI(TAG, "scale_split: %dx%d poses=%d tris=%lld frame mean/p50/max us %lld/%ld/%ld | %s", size.width,
             size.height, poses, (long long)(triangles / poses), (long long)(sum / poses), (long)frame_us[poses / 2],
             (long)frame_us[poses - 1], report);
    ESP_LOGI(TAG, "scale_spans: %dx%d one core us/pose setup %lld rows %lld span_setup %lld fill %lld clear %lld",
             size.width, size.height, (long long)(spans.setup / poses), (long long)(spans.rows / poses),
             (long long)(spans.span_setup / poses), (long long)(spans.fill / poses), (long long)(spans.clear / poses));
}

/* Whether step `step` of ladder `ladder` is a size an earlier step, of this
 * ladder or one before it, already measured. */
static bool
measured_before(int ladder, int step) {
    const resolution_step_t* size = &ladders[ladder].steps[step];
    for (int l = 0; l <= ladder; l++) {
        for (int i = 0; i < (l == ladder ? step : ladders[l].count); i++) {
            if (ladders[l].steps[i].width == size->width && ladders[l].steps[i].height == size->height) {
                return true;
            }
        }
    }
    return false;
}

void
test_raster_stage_split_by_size(void) {
#if !FRAME_COST_ENABLED
    TEST_IGNORE_MESSAGE("frame_cost is a development build's");
#endif
    TEST_ASSERT_NOT_NULL_MESSAGE(scene, "the scene did not load: see the log above");
    const r3d_instance_t instance = {&mesh, NULL};
    raster_t raster = {
        .instances = &instance,
        .instance_count = 1,
        .upscaled = true,
        .destination_width = GFX_WIDTH,
        .destination_height = GFX_HEIGHT,
    };
    raster.width = ladders[0].steps[0].width; /* every size is no larger */
    raster.height = ladders[0].steps[0].height;
    const size_t scratch_bytes = raster_scratch_bytes(&raster);
    raster.scratch = memory_alloc(scratch_bytes, MEMORY_PSRAM);
    raster.destination = memory_alloc(sizeof(gfx_color_t) * (size_t)GFX_WIDTH * GFX_HEIGHT, MEMORY_PSRAM);
    int32_t* frame_us = malloc(sizeof(int32_t) * POSES_MAX);
    char* report = malloc(FRAME_COST_REPORT_MAX); /* too big for the frame task's stack */
    TEST_ASSERT_NOT_NULL(report);
    TEST_ASSERT_NOT_NULL(raster.scratch);
    TEST_ASSERT_NOT_NULL(raster.destination);
    TEST_ASSERT_NOT_NULL(frame_us);
    for (int l = 0; l < LADDER_COUNT; l++) {
        for (int i = 0; i < ladders[l].count; i++) {
            if (!measured_before(l, i)) {
                measure_size(&raster, ladders[l].steps[i], frame_us, report);
            }
        }
    }
    for (int i = 0; i < (int)(sizeof mapped_sizes / sizeof mapped_sizes[0]); i++) {
        measure_size(&raster, mapped_sizes[i], frame_us, report);
    }
    free(report);
    free(frame_us);
    memory_free(raster.destination);
    memory_free(raster.scratch);
    TEST_PASS();
}

#define FRAME_DT_MS 50
#define FRAMES_MAX  1024
#define LINE_FRAMES 40

typedef struct {
    int8_t step;
    int32_t draw_us, upscale_us;
    int32_t triangles;
} frame_record_t;

/* Every step at every pose, drawn as the scene draws it, for the fit. */
static bool
calibrate(const resolution_config_t* config, resolution_model_t* model) {
    const r3d_instance_t instance = {&mesh, NULL};
    raster_t raster = {.instances = &instance,
                       .instance_count = 1,
                       .upscaled = true,
                       .width = config->steps[0].width,
                       .height = config->steps[0].height,
                       .destination_width = GFX_WIDTH,
                       .destination_height = GFX_HEIGHT};
    const uint32_t period = r3d_scene_camera_period_ms(path);
    const int poses = (int)((period + POSE_EVERY_MS - 1) / POSE_EVERY_MS);
    raster.scratch = memory_alloc(raster_scratch_bytes(&raster), MEMORY_PSRAM);
    raster.destination = memory_alloc(sizeof(gfx_color_t) * (size_t)GFX_WIDTH * GFX_HEIGHT, MEMORY_PSRAM);
    resolution_sample_t* samples = memory_alloc(sizeof(*samples) * (size_t)(poses * config->step_count), MEMORY_PSRAM);
    bool fitted = false;
    if (raster.scratch != NULL && raster.destination != NULL && samples != NULL) {
        int count = 0;
        for (int step = 0; step < config->step_count; step++) {
            raster.width = config->steps[step].width;
            raster.height = config->steps[step].height;
            for (uint32_t t_ms = 0; t_ms < period; t_ms += POSE_EVERY_MS) {
                const camera_t camera = r3d_scene_camera_at(path, t_ms);
                const int64_t start = timing_now_us();
                const raster_stats_t stats = raster_draw(&raster, &camera, 0);
                const int64_t drawn = timing_now_us();
                raster_upscale(&raster);
                samples[count++] = (resolution_sample_t){step, stats.triangles, (int32_t)(drawn - start),
                                                         (int32_t)(timing_now_us() - drawn)};
            }
        }
        fitted = resolution_model_fit(model, config, samples, count);
    }
    memory_free(samples);
    memory_free(raster.destination);
    memory_free(raster.scratch);
    return fitted;
}

static void
log_frames(const char* policy, const char* ladder, int32_t budget_us, const frame_record_t* records, int frames) {
    const size_t line_size = (LINE_FRAMES * 32) + 64;
    char* line = malloc(line_size);
    TEST_ASSERT_NOT_NULL(line);
    for (int first = 0; first < frames; first += LINE_FRAMES) {
        int length = snprintf(line, line_size, "dynres_frames: %s %s %ld %d", policy, ladder, (long)budget_us, first);
        for (int i = first; i < frames && i < first + LINE_FRAMES && length > 0 && (size_t)length < line_size; i++) {
            const frame_record_t* r = &records[i];
            length += snprintf(line + length, line_size - (size_t)length, " %d:%ld:%ld:%ld", r->step, (long)r->draw_us,
                               (long)r->upscale_us, (long)r->triangles);
        }
        ESP_LOGI(TAG, "%s", line);
    }
    free(line);
}

/* The path flown through the scene manager at FRAME_DT_MS a frame: at the
 * camera's half scale when `config` is NULL, else under the policy. */
static void
fly(const char* policy, const ladder_t* ladder, const resolution_config_t* config, const resolution_model_t* model,
    uint16_t* picture, frame_record_t* records, int frames) {
    scene_t* flown = scene_load(SPONZA_SCENE, NULL);
    TEST_ASSERT_NOT_NULL(flown);
    for (int i = 0; i < (int)SPONZA_BAKE_COUNT; i++) {
        scene_entity_set_enabled(flown, scene_find(flown, sponza_bakes[i]), i == (int)SPONZA_BAKE_FULL);
    }
    TEST_ASSERT_TRUE(scene_activate(flown, NULL));
    render_context_set_scale(render_context_main(), 50);
    render_context_set_dynamic_resolution(render_context_main(), config, model, ladder == NULL ? 0 : ladder->half);
    const scene_target_t target = {picture, GFX_WIDTH, GFX_HEIGHT};
    /* Timed here, around the scene's own two calls, so every policy and the
     * fixed scale (which the context does not time) are timed alike. */
    for (int i = 0; i < frames; i++) {
        const int64_t began_us = timing_now_us();
        scene_render(FRAME_DT_MS, 0, GFX_WIDTH, GFX_HEIGHT);
        const int64_t drawn_us = timing_now_us();
        scene_compose(FRAME_DT_MS, 0, &target);
        const int64_t composed_us = timing_now_us();
        const render_context_frame_t r = render_context_frame(render_context_main());
        records[i] = (frame_record_t){(int8_t)r.step, (int32_t)(drawn_us - began_us), (int32_t)(composed_us - drawn_us),
                                      r.stats.triangles};
    }
    render_context_set_dynamic_resolution(render_context_main(), NULL, NULL, 0);
    scene_unload(flown);
    log_frames(policy, ladder == NULL ? "half" : ladder->name, config == NULL ? 0 : config->budget_us, records, frames);
}

/* A ladder's config at `budget_us`; not inlined, so the config it returns is
 * off the stack before the calibration frames draw. */
static __attribute__((noinline)) void
configure(const ladder_t* ladder, int32_t budget_us, resolution_config_t* config) {
    *config = resolution_config(ladder->steps, ladder->count, ladder->recovery_from, budget_us);
}

/* The fitted model's lines; not inlined, so its frame is gone before the
 * calibration frames draw. */
static __attribute__((noinline)) void
log_fit(const ladder_t* ladder, const resolution_config_t* config, const resolution_model_t* model) {
    ESP_LOGI(TAG, "dynres_model: %s base %.0f per_triangle %.4f per_triangle_row %.4f per_pixel_share %.0f",
             ladder->name, (double)model->base_us, (double)model->per_triangle_us, (double)model->per_triangle_row_us,
             (double)model->per_pixel_share_us);
    for (int step = 0; step < ladder->count; step++) {
        ESP_LOGI(TAG, "dynres_step: %s %d %dx%d upscale %.0f", ladder->name, step, config->steps[step].width,
                 config->steps[step].height, (double)model->upscale_us[step]);
    }
}

/* A ladder's config at `budget_us`, fitted to the board on every one of its
 * steps; not inlined, so its frame is gone before the flights run. */
static __attribute__((noinline)) void
fit_ladder(const ladder_t* ladder, int32_t budget_us, resolution_config_t* config, resolution_model_t* model) {
    configure(ladder, budget_us, config);
    TEST_ASSERT_TRUE_MESSAGE(calibrate(config, model), "the calibration frames did not fit a model");
    log_fit(ladder, config, model);
}

void
test_dynamic_resolution_policies_along_the_path(void) {
    TEST_ASSERT_NOT_NULL_MESSAGE(scene, "the scene did not load: see the log above");
    static const int32_t budgets_us[] = {60000, 75000};

    /* Each ladder's config and fitted model: too big for the frame task's stack. */
    typedef struct {
        resolution_config_t config;
        resolution_model_t model;
    } fitted_t;

    fitted_t* fitted = memory_alloc(sizeof(*fitted) * LADDER_COUNT, MEMORY_PSRAM);
    const int path_frames = (int)(r3d_scene_camera_period_ms(path) / FRAME_DT_MS);
    const int frames = path_frames < FRAMES_MAX ? path_frames : FRAMES_MAX;
    uint16_t* picture = memory_alloc(sizeof(uint16_t) * (size_t)GFX_WIDTH * GFX_HEIGHT, MEMORY_PSRAM);
    frame_record_t* records = memory_alloc(sizeof(*records) * FRAMES_MAX, MEMORY_PSRAM);
    TEST_ASSERT_NOT_NULL(fitted);
    TEST_ASSERT_NOT_NULL(picture);
    TEST_ASSERT_NOT_NULL(records);
    for (int i = 0; i < LADDER_COUNT; i++) {
        fit_ladder(&ladders[i], budgets_us[0], &fitted[i].config, &fitted[i].model);
    }
    fly("fixed", NULL, NULL, NULL, picture, records, frames);
    for (int b = 0; b < (int)(sizeof budgets_us / sizeof budgets_us[0]); b++) {
        for (int i = 0; i < LADDER_COUNT; i++) {
            fitted[i].config.budget_us = budgets_us[b];
        }
        fly("stepped", &ladders[0], &fitted[0].config, NULL, picture, records, frames);
        fly("predicted", &ladders[0], &fitted[0].config, &fitted[0].model, picture, records, frames);
        fly("predicted", &ladders[1], &fitted[1].config, &fitted[1].model, picture, records, frames);
    }
    memory_free(records);
    memory_free(picture);
    memory_free(fitted);
    TEST_PASS();
}

void
run_raster_scale_perf_suite(void) {
    scene_failure_t why;
    scene = scene_load(SPONZA_SCENE, &why);
    if (scene == NULL) {
        ESP_LOGE(TAG, "scene: status %d, asset %s, about '%s'", (int)why.status, asset_status_text(why.asset),
                 why.what);
    } else {
        pack = asset_store_pack(SPONZA_SCENE);
        path = scene_camera_lens(scene, NULL);
        const scene_entity_t full = scene_find(scene, sponza_bakes[SPONZA_BAKE_FULL]);
        const char* id = full == SCENE_ENTITY_NONE ? NULL : scene_entity_mesh_id(scene, full);
        if (id == NULL || r3d_lit_mesh_open(pack, id, &mesh) != ASSET_OK) {
            ESP_LOGE(TAG, "the scene's full bake did not open");
            scene_unload(scene);
            scene = NULL;
        }
    }
    RUN_TEST(test_raster_stage_split_by_size);
    RUN_TEST(test_dynamic_resolution_policies_along_the_path);
    if (pack != NULL) {
        asset_store_release(SPONZA_SCENE);
    }
    scene_unload(scene);
    scene = NULL;
    pack = NULL;
    path = NULL;
}

#else /* !DEVICE_BUILD */

void
run_raster_scale_perf_suite(void) {}

#endif /* DEVICE_BUILD */

SUITE_REGISTER_ON_REQUEST(run_raster_scale_perf_suite);
