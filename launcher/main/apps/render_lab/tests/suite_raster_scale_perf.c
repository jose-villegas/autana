/*
 * Device-only suite: where a raster frame's time goes at each render size,
 * along the render lab's flythrough. One line per size gives the raster's
 * frame_cost stages on both cores and one core's span stages, which is what
 * a dynamic-resolution controller's steps and cost model are sized from.
 * The first camera path supplies the offline fit. Then each policy flies
 * every named camera path through the scene manager at a fixed frame step,
 * one record per frame, against two budgets. Logs include frame costs,
 * chosen predictions, the refit's final scale and offset and its cost.
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

#include "apps/render_lab/render_lab_dynres.h"
#include "apps/render_lab/render_lab_view.h"
#include "apps/render_lab/sponza_content.h"
#include "apps/render_lab/tests/one_core_draw.h"
#include "apps/render_lab/tests/r3d_perf_quarters.h"
#include "asset/asset_store.h"
#include "collections/strbuf.h"
#include "core/memory.h"
#include "core/timing.h"
#include "display/viewport.h"
#include "gfx/gfx.h"
#include "math/scalar/units.h"
#include "math/stats/statsi.h"
#include "profile/frame_cost.h"
#include "render/context/render_context.h"
#include "render/r3d_lit_mesh.h"
#include "render/r3d_span_internal.h"
#include "render/raster_internal.h"
#include "render/resolution/resolution.h"
#include "scene/scene.h"
#include "scene/scene_shell.h"
#include "sponza_suite.h"

static const r3d_perf_quarter_t* perf_quarter;

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
    {"height", render_lab_ladder, RENDER_LAB_LADDER_STEPS, RENDER_LAB_LADDER_STEPS, RENDER_LAB_LADDER_HALF},
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
static const camera_t* lens;
static scene_entity_t camera_entity;
static r3d_lit_mesh_t mesh;

typedef struct {
    int64_t setup, edges, carry, fill, clear;
    r3d_lens_t lens;
    raster_buffers_t buffers;
} span_split_t;

/* One core, one pose: each stage as the difference of two stopped draws;
 * not inlined, so its frame is gone before the raster draws. */
static __attribute__((noinline)) void
add_span_split(raster_t* raster, uint32_t t_ms, span_split_t* sum) {
    (void)scene_animator_seek(scene, camera_entity, SCENE_ANIMATOR_CURRENT, t_ms);
    r3d_lens_t* draw_lens = &sum->lens;
    {
        const render_view_t view =
            sponza_suite_view(lens, scene, camera_entity, (viewport_t){GFX_WIDTH, GFX_HEIGHT, perf_quarter->quarter});
        r3d_lens_init(draw_lens, &view, mesh.position_scale);
    }
    r3d_lens_fit(draw_lens, raster->width, raster->height);
    sum->buffers = raster_carve(raster);
    const raster_buffers_t* b = &sum->buffers;
    const int visible = r3d_pipeline_cull(&mesh, draw_lens, b->culled + 1, b->work[0], NULL);
    r3d_pipeline_transform(&mesh, draw_lens, b->culled + 1, visible, b->cs, b->rows);
    const size_t pixels = (size_t)raster->width * (size_t)raster->height;
    const int64_t clear_from = timing_now_us();
    memset(gfx_target_depth(&b->picture, 0), 0, pixels * sizeof(uint16_t));
    sum->clear += timing_now_us() - clear_from;
    const int64_t setup = one_core_draw_us(raster, b, draw_lens, visible, R3D_SPAN_STOP_AFTER_SETUP);
    const int64_t edges = one_core_draw_us(raster, b, draw_lens, visible, R3D_SPAN_STOP_AFTER_EDGES);
    const int64_t carry = one_core_draw_us(raster, b, draw_lens, visible, R3D_SPAN_STOP_AFTER_CARRY);
    const int64_t whole = one_core_draw_us(raster, b, draw_lens, visible, R3D_SPAN_STOP_NONE);
    sum->setup += setup;
    sum->edges += edges - setup;
    sum->carry += carry - edges;
    sum->fill += whole - carry;
}

static int64_t
measure_size(raster_t* raster, gfx_color_t* destination, resolution_step_t size, int32_t* frame_us, char* report) {
    size = raster_present_size(size, render_lab_present());
    raster->width = size.width;
    raster->height = size.height;
    (void)frame_cost_take_report(1, report, FRAME_COST_REPORT_MAX); /* forget whatever ran before */
    const uint32_t period = scene_animator_clip_duration_ms(scene, camera_entity, 0);
    int poses = 0;
    int64_t triangles = 0;
    for (uint32_t t_ms = 0; t_ms < period && poses < POSES_MAX; t_ms += POSE_EVERY_MS) {
        (void)scene_animator_seek(scene, camera_entity, SCENE_ANIMATOR_CURRENT, t_ms);
        const render_view_t view =
            sponza_suite_view(lens, scene, camera_entity, (viewport_t){GFX_WIDTH, GFX_HEIGHT, perf_quarter->quarter});
        const int64_t start = timing_now_us();
        const raster_stats_t stats = raster_draw(raster, &view);
        raster->present = render_lab_present();
        const resolution_step_t composed = raster_compose_size(raster);
        raster_upscale(raster, destination, composed.width, composed.height);
        frame_us[poses++] = (int32_t)(timing_now_us() - start);
        triangles += stats.triangles;
    }
    (void)frame_cost_take_report((uint32_t)poses, report, FRAME_COST_REPORT_MAX);

    span_split_t* spans = memory_alloc(sizeof(*spans), MEMORY_INTERNAL);
    TEST_ASSERT_NOT_NULL(spans);
    *spans = (span_split_t){0};
    for (uint32_t t_ms = 0; t_ms < period && t_ms / POSE_EVERY_MS < POSES_MAX; t_ms += POSE_EVERY_MS) {
        add_span_split(raster, t_ms, spans);
    }

    int64_t sum = 0;
    for (int i = 0; i < poses; i++) {
        sum += frame_us[i];
    }
    ESP_LOGI(TAG,
             "scale_split: %dx%d poses=%d tris=%lld frame mean/p50/max us %lld/%ld/%ld | %s" R3D_PERF_QUARTER_FORMAT,
             size.width, size.height, poses, (long long)(triangles / poses), (long long)(sum / poses),
             (long)statsi_median(frame_us, (size_t)poses), (long)statsi_percentile(frame_us, (size_t)poses, 100),
             report, perf_quarter->name);
    ESP_LOGI(TAG,
             "scale_spans: %dx%d one core us/pose setup %lld edges %lld carry %lld fill %lld clear "
             "%lld" R3D_PERF_QUARTER_FORMAT,
             size.width, size.height, (long long)(spans->setup / poses), (long long)(spans->edges / poses),
             (long long)(spans->carry / poses), (long long)(spans->fill / poses), (long long)(spans->clear / poses),
             perf_quarter->name);
    memory_free(spans);
    return sum / poses;
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
    gfx_color_t* destination = memory_alloc(GFX_FRAMEBUFFER_BYTES, MEMORY_PSRAM);
    int32_t* frame_us = malloc(sizeof(int32_t) * POSES_MAX);
    char* report = malloc(FRAME_COST_REPORT_MAX); /* too big for the frame task's stack */
    TEST_ASSERT_NOT_NULL(report);
    TEST_ASSERT_NOT_NULL(raster.scratch);
    TEST_ASSERT_NOT_NULL(destination);
    TEST_ASSERT_NOT_NULL(frame_us);
    for (int l = 0; l < LADDER_COUNT; l++) {
        for (int i = 0; i < ladders[l].count; i++) {
            if (!measured_before(l, i)) {
                measure_size(&raster, destination, ladders[l].steps[i], frame_us, report);
            }
        }
    }
    for (int i = 0; i < (int)(sizeof mapped_sizes / sizeof mapped_sizes[0]); i++) {
        measure_size(&raster, destination, mapped_sizes[i], frame_us, report);
    }
    free(report);
    free(frame_us);
    memory_free(destination);
    memory_free(raster.scratch);
    TEST_PASS();
}

static const char* const counter_events[] = {"d_stall_all", "i_stall_busy"};

/* `event` armed on r3d.draw along the path; the counts line into `report`. */
static void
count_along_path(raster_t* raster, gfx_color_t* destination, int bracket, const char* event, char* report) {
    const int index = frame_cost_event_index(event);
    TEST_ASSERT_TRUE(index >= 0);
    frame_cost_shared_post_arm(bracket, index);
    const uint32_t period = scene_animator_clip_duration_ms(scene, camera_entity, 0);
    for (uint32_t t_ms = 0; t_ms < period && t_ms / POSE_EVERY_MS < POSES_MAX; t_ms += POSE_EVERY_MS) {
        (void)scene_animator_seek(scene, camera_entity, SCENE_ANIMATOR_CURRENT, t_ms);
        const render_view_t view =
            sponza_suite_view(lens, scene, camera_entity, raster_viewport(raster, perf_quarter->quarter));
        (void)raster_draw(raster, &view);
        raster->present = render_lab_present();
        const resolution_step_t composed = raster_compose_size(raster);
        raster_upscale(raster, destination, composed.width, composed.height);
    }
    const int length = frame_cost_take_counts(report, FRAME_COST_REPORT_MAX);
    frame_cost_shared_post_arm(-1, 0);
    const int mark = frame_cost_begin();
    (void)frame_cost_end(mark, "counters.disarm");
    TEST_ASSERT_TRUE_MESSAGE(length > 0, "the draw counter did not arm");
}

static void
test_raster_counters(void) {
#if !FRAME_COST_ENABLED
    TEST_IGNORE_MESSAGE("frame_cost is a development build's");
#else
    TEST_ASSERT_NOT_NULL_MESSAGE(scene, "the scene did not load: see the log above");
    const r3d_instance_t instance = {&mesh, NULL};
    const resolution_step_t size = render_lab_render_size(GFX_WIDTH, GFX_HEIGHT);
    raster_t raster = {.instances = &instance,
                       .instance_count = 1,
                       .upscaled = true,
                       .width = size.width,
                       .height = size.height,
                       .destination_width = GFX_WIDTH,
                       .destination_height = GFX_HEIGHT};
    ESP_LOGI(TAG, "raster=%dx%d present=%d" R3D_PERF_QUARTER_FORMAT, raster.width, raster.height, render_lab_present(),
             perf_quarter->name);
    raster.scratch = memory_alloc(raster_scratch_bytes(&raster), MEMORY_PSRAM);
    gfx_color_t* destination = memory_alloc(GFX_FRAMEBUFFER_BYTES, MEMORY_PSRAM);
    int32_t* frame_us = malloc(sizeof(*frame_us) * POSES_MAX);
    char* report = malloc(FRAME_COST_REPORT_MAX);
    TEST_ASSERT_NOT_NULL(raster.scratch);
    TEST_ASSERT_NOT_NULL(destination);
    TEST_ASSERT_NOT_NULL(frame_us);
    TEST_ASSERT_NOT_NULL(report);
    const int64_t mean_us = measure_size(&raster, destination, size, frame_us, report);
    ESP_LOGI(TAG, "raster both cores: mean %lldus" R3D_PERF_QUARTER_FORMAT, (long long)mean_us, perf_quarter->name);
    const int bracket = frame_cost_shared_name_index("r3d.draw");
    TEST_ASSERT_TRUE(bracket >= 0);
    /* Counters cover core 0's half of the draw. */
    for (size_t event = 0; event < sizeof counter_events / sizeof counter_events[0]; event++) {
        count_along_path(&raster, destination, bracket, counter_events[event], report);
        ESP_LOGI(TAG, "%s" R3D_PERF_QUARTER_FORMAT, report, perf_quarter->name);
    }
    free(report);
    free(frame_us);
    memory_free(destination);
    memory_free(raster.scratch);
#endif
}

#define FRAME_DT_MS 50
#define FRAMES_MAX  1024
#define LINE_FRAMES 40

typedef struct {
    int8_t step;
    int32_t draw_us, upscale_us;
    int32_t triangles;
    int32_t predicted_us;
} frame_record_t;

/* Every step at every pose, drawn as the scene draws it, for the fit. */
static bool
calibrate(const resolution_config_t* config, resolution_model_t* model) {
    const r3d_instance_t instance = {&mesh, NULL};
    render_context_t* context = memory_alloc(sizeof(*context), MEMORY_INTERNAL);
    if (context == NULL) {
        return false;
    }
    memset(context, 0, sizeof(*context));
    context->policy = RENDER_FIXED;
    raster_t* raster = &context->raster;
    raster->instances = &instance;
    raster->instance_count = 1;
    raster->upscaled = true;
    raster->present = render_lab_present();
    raster->width = config->steps[0].width;
    raster->height = config->steps[0].height;
    raster->destination_width = GFX_WIDTH;
    raster->destination_height = GFX_HEIGHT;
    const uint32_t period = scene_animator_clip_duration_ms(scene, camera_entity, 0);
    const int poses = (int)((period + POSE_EVERY_MS - 1) / POSE_EVERY_MS);
    raster->scratch = memory_alloc(raster_scratch_bytes(raster), MEMORY_PSRAM);
    gfx_color_t* destination = memory_alloc(GFX_FRAMEBUFFER_BYTES, MEMORY_PSRAM);
    gfx_color_t* half = memory_alloc(sizeof(*half) * (GFX_WIDTH / 2) * (GFX_HEIGHT / 2), MEMORY_PSRAM);
    resolution_sample_t* samples = memory_alloc(sizeof(*samples) * (size_t)(poses * config->step_count), MEMORY_PSRAM);
    render_view_t* view = memory_alloc(sizeof(*view), MEMORY_INTERNAL);
    bool fitted = false;
    if (raster->scratch != NULL && destination != NULL && half != NULL && samples != NULL && view != NULL) {
        int count = 0;
        for (int step = 0; step < config->step_count; step++) {
            raster->width = config->steps[step].width;
            raster->height = raster_present_size(config->steps[step], render_lab_present()).height;
            for (uint32_t t_ms = 0; t_ms < period; t_ms += POSE_EVERY_MS) {
                (void)scene_animator_seek(scene, camera_entity, SCENE_ANIMATOR_CURRENT, t_ms);
                *view = sponza_suite_view(lens, scene, camera_entity,
                                          (viewport_t){GFX_WIDTH, GFX_HEIGHT, perf_quarter->quarter});
                const int64_t start = timing_now_us();
                const raster_stats_t stats = raster_draw(raster, view);
                const int64_t drawn = timing_now_us();
                (void)render_context_compose(context, destination, half, NULL);
                samples[count++] = (resolution_sample_t){step, stats.triangles, (int32_t)(drawn - start),
                                                         (int32_t)(timing_now_us() - drawn)};
            }
        }
        fitted = resolution_model_fit(model, config, samples, count);
    }
    memory_free(view);
    memory_free(samples);
    memory_free(half);
    memory_free(destination);
    memory_free(raster->scratch);
    memory_free(context);
    return fitted;
}

static __attribute__((noinline)) void
log_frames(const char* camera, const char* policy, const char* ladder, int32_t budget_us, const frame_record_t* records,
           int frames) {
    const size_t line_size = (LINE_FRAMES * 64) + 128;
    char* line = malloc(line_size);
    TEST_ASSERT_NOT_NULL(line);
    for (int first = 0; first < frames; first += LINE_FRAMES) {
        strbuf_t text = strbuf_make(line, line_size);
        strbuf_appendf(&text, "dynres_frames: %s %s %s %ld %d", camera, policy, ladder, (long)budget_us, first);
        for (int i = first; i < frames && i < first + LINE_FRAMES; i++) {
            const frame_record_t* r = &records[i];
            strbuf_appendf(&text, " %d:%ld:%ld:%ld:%ld", r->step, (long)r->draw_us, (long)r->upscale_us,
                           (long)r->triangles, (long)r->predicted_us);
        }
        ESP_LOGI(TAG, "%s" R3D_PERF_QUARTER_FORMAT, line, perf_quarter->name);
    }
    free(line);
}

/* Frame metadata stays off the stack while the next frame draws. */
static __attribute__((noinline)) void
record_frame(frame_record_t* record, int64_t began_us, int64_t drawn_us, int64_t composed_us) {
    const render_context_frame_t r = render_context_frame(render_context_main());
    *record = (frame_record_t){(int8_t)r.step, (int32_t)(drawn_us - began_us), (int32_t)(composed_us - drawn_us),
                               r.stats.triangles, r.predicted_us};
}

/* Where the predictor's refit ended a flight; not inlined, so its frame is
 * not on the stack while the flight draws. */
static __attribute__((noinline)) void
log_refit(const char* camera, const ladder_t* ladder, const resolution_config_t* config) {
    const resolution_predict_t* refit = &render_context_main()->predict;
    ESP_LOGI(TAG, "dynres_refit: %s %s %ld scale %.3f offset_us %.0f" R3D_PERF_QUARTER_FORMAT, camera, ladder->name,
             (long)config->budget_us, (double)refit->scale, (double)refit->offset_us, perf_quarter->name);
}

/* The path flown through the scene manager at FRAME_DT_MS a frame: at the
 * camera's half scale when `config` is NULL, else under the policy. */
static __attribute__((noinline)) void
fly(const char* camera, const char* policy, const ladder_t* ladder, const resolution_config_t* config,
    const resolution_model_t* model, uint16_t* picture, frame_record_t* records) {
    scene_t* flown = scene_load(SPONZA_SCENE, NULL);
    TEST_ASSERT_NOT_NULL(flown);
    for (int i = 0; i < (int)SCENE_BAKED_VARIANT_COUNT; i++) {
        scene_entity_set_enabled(flown, scene_find(flown, name_hash(sponza_bakes[i])), i == (int)SCENE_BAKED_FULL);
    }
    const name_hash_t camera_name = name_hash(camera);
    const scene_entity_t flown_camera = scene_find(flown, camera_name);
    TEST_ASSERT_TRUE(scene_activate(flown, camera_name));
    TEST_ASSERT_TRUE(render_lab_apply_camera_settings(flown, flown_camera));
    TEST_ASSERT_NOT_NULL(scene_camera_lens(flown, camera_name));
    const uint32_t period = scene_animator_clip_duration_ms(flown, flown_camera, 0);
    const int frames = (int)((period + FRAME_DT_MS - 1) / FRAME_DT_MS);
    TEST_ASSERT_TRUE(frames > 0 && frames <= FRAMES_MAX);
    render_lab_apply_raster_settings();
    render_context_set_dynamic_resolution(render_context_main(), config, model, ladder == NULL ? 0 : ladder->half);
    scene_target_t target = {picture, GFX_WIDTH, GFX_HEIGHT, NULL, NULL};
    gfx_color_t* half = memory_alloc(sizeof(*half) * (GFX_WIDTH / 2) * (GFX_HEIGHT / 2), MEMORY_PSRAM);
    TEST_ASSERT_NOT_NULL(half);
    target.half_pixels = half;
    /* Timed here, around the scene's own two calls, so every policy and the
     * fixed scale (which the context does not time) are timed alike. */
    for (int i = 0; i < frames; i++) {
        const int64_t began_us = timing_now_us();
        scene_render(FRAME_DT_MS, perf_quarter->quarter, GFX_WIDTH, GFX_HEIGHT);
        const int64_t drawn_us = timing_now_us();
        scene_compose(FRAME_DT_MS, perf_quarter->quarter, &target);
        const int64_t composed_us = timing_now_us();
        record_frame(&records[i], began_us, drawn_us, composed_us);
    }
    if (model != NULL) {
        log_refit(camera, ladder, config);
    }
    render_context_set_dynamic_resolution(render_context_main(), NULL, NULL, 0);
    memory_free(half);
    scene_unload(flown);
    log_frames(camera, policy, ladder == NULL ? "half" : ladder->name, config == NULL ? 0 : config->budget_us, records,
               frames);
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
    ESP_LOGI(TAG,
             "dynres_model: %s base %.0f per_triangle %.4f per_triangle_row %.4f per_pixel_share "
             "%.0f" R3D_PERF_QUARTER_FORMAT,
             ladder->name, (double)model->base_us, (double)model->per_triangle_us, (double)model->per_triangle_row_us,
             (double)model->per_pixel_share_us, perf_quarter->name);
    for (int step = 0; step < ladder->count; step++) {
        ESP_LOGI(TAG, "dynres_step: %s %d %dx%d upscale %.0f" R3D_PERF_QUARTER_FORMAT, ladder->name, step,
                 config->steps[step].width, config->steps[step].height, (double)model->upscale_us[step],
                 perf_quarter->name);
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

static const int32_t budgets_us[] = {RENDER_LAB_TARGET_MS * MATH_US_PER_MS, 75000};

void
test_dynamic_resolution_policies_along_the_path(void) {
    TEST_ASSERT_NOT_NULL_MESSAGE(scene, "the scene did not load: see the log above");

    /* Each ladder's config and fitted model: too big for the frame task's stack. */
    typedef struct {
        resolution_config_t config;
        resolution_model_t model;
    } fitted_t;

    fitted_t* fitted = memory_alloc(sizeof(*fitted) * LADDER_COUNT, MEMORY_PSRAM);
    uint16_t* picture = memory_alloc(GFX_FRAMEBUFFER_BYTES, MEMORY_PSRAM);
    frame_record_t* records = memory_alloc(sizeof(*records) * FRAMES_MAX, MEMORY_PSRAM);
    TEST_ASSERT_NOT_NULL(fitted);
    TEST_ASSERT_NOT_NULL(picture);
    TEST_ASSERT_NOT_NULL(records);
    for (int i = 0; i < LADDER_COUNT; i++) {
        fit_ladder(&ladders[i], budgets_us[0], &fitted[i].config, &fitted[i].model);
    }
    for (int camera = 0; camera < SPONZA_CAMERA_COUNT; camera++) {
        const char* name = sponza_cameras[camera];
        fly(name, "fixed", NULL, NULL, NULL, picture, records);
        for (int b = 0; b < (int)(sizeof budgets_us / sizeof budgets_us[0]); b++) {
            for (int i = 0; i < LADDER_COUNT; i++) {
                fitted[i].config.budget_us = budgets_us[b];
            }
            fly(name, "stepped", &ladders[0], &fitted[0].config, NULL, picture, records);
            fly(name, "predicted", &ladders[0], &fitted[0].config, &fitted[0].model, picture, records);
            fly(name, "predicted", &ladders[1], &fitted[1].config, &fitted[1].model, picture, records);
        }
    }
    memory_free(records);
    memory_free(picture);
    memory_free(fitted);
    TEST_PASS();
}

static void
test_dynamic_resolution_refit_cost(void) {
    static const resolution_model_t prior = {10000.0F, 2.0F, 4.0F, 20000.0F, {0}};
    resolution_predict_t* predict = memory_alloc(sizeof(*predict), MEMORY_INTERNAL);
    resolution_config_t* config = memory_alloc(sizeof(*config), MEMORY_INTERNAL);
    TEST_ASSERT_NOT_NULL(predict);
    TEST_ASSERT_NOT_NULL(config);
    configure(&ladders[0], budgets_us[0], config);
    resolution_predict_init(predict, config, &prior, 0);
    int64_t total_us = 0;
    int64_t max_us = 0;
    for (int i = 0; i < 1000; i++) {
        predict->step = i % config->step_count;
        const int triangles = 1000 + (i * 137 % 19000);
        const int32_t frame_us = (int32_t)resolution_model_predict_us(&prior, config, predict->step, triangles);
        const int64_t began_us = timing_now_us();
        resolution_predict_measured(predict, config, triangles, frame_us);
        const int64_t elapsed_us = timing_now_us() - began_us;
        total_us += elapsed_us;
        max_us = elapsed_us > max_us ? elapsed_us : max_us;
    }
    ESP_LOGI(TAG, "dynres_refit_cost: calls 1000 mean_us %.3f max_us %lld", (double)total_us / 1000.0,
             (long long)max_us);
    memory_free(config);
    memory_free(predict);
    TEST_PASS();
}

void
run_raster_scale_perf_suite(void) {
    sponza_suite_t loaded = sponza_suite_load();
    scene = loaded.scene;
    pack = asset_store_loaded(loaded.load);
    lens = loaded.lens;
    camera_entity = scene == NULL ? SCENE_ENTITY_NONE : scene_find(scene, name_hash(sponza_cameras[0]));
    (void)render_lab_apply_camera_settings(scene, camera_entity);
    if (scene != NULL) {
        const scene_entity_t full = scene_find(scene, name_hash(sponza_bakes[SCENE_BAKED_FULL]));
        const char* id = full == SCENE_ENTITY_NONE ? NULL : scene_entity_mesh_id(scene, full);
        if (id == NULL || r3d_lit_mesh_open(pack, id, &mesh) != ASSET_OK) {
            ESP_LOGE(TAG, "the scene's full bake did not open");
            scene_unload(scene);
            scene = NULL;
        }
    }
    RUN_TEST(test_dynamic_resolution_refit_cost);
    for (size_t q = 0; q < R3D_PERF_QUARTER_COUNT; q++) {
        perf_quarter = &r3d_perf_quarters[q];
        RUN_TEST(test_raster_stage_split_by_size);
        RUN_TEST(test_raster_counters);
        RUN_TEST(test_dynamic_resolution_policies_along_the_path);
    }
    loaded = (sponza_suite_t){scene, loaded.load, lens, loaded.arena_mark};
    sponza_suite_release(&loaded);
    scene = NULL;
    pack = NULL;
    lens = NULL;
}

#else /* !DEVICE_BUILD */

void
run_raster_scale_perf_suite(void) {}

#endif /* DEVICE_BUILD */

SUITE_REGISTER_ON_REQUEST(run_raster_scale_perf_suite);
