#include "apps/render_lab/tests/render_lab_test.h"
#ifdef DEVICE_BUILD
#include <stdio.h>
#include <string.h>
#include "apps/render_lab/render_lab.h"
#include "apps/render_lab/render_lab_dynres.h"
#include "apps/render_lab/render_lab_view.h"
#include "apps/render_lab/tests/r3d_perf_quarters.h"
#include "collections/strbuf.h"
#include "core/memory.h"
#include "core/timing.h"
#include "core/tune.h"
#include "esp_log.h"
#include "frame/test_frame.h"
#include "frame_profile.h"
#include "gfx_shared_state.h"
#include "profile/frame_cost.h"
#include "render/context/render_context.h"
#include "render/view/raster_cull_view.h"
#include "scene/animator/animator.h"
#include "scene/scene_internal.h"
#include "scene/scene_shell.h"
#include "ui/ui.h"
#include "unity.h"

const char*
render_lab_test_step_name(int step) {
    static char name[sizeof "-2147483648x-2147483648"];
    const resolution_step_t size = render_lab_ladder[step];
    (void)snprintf(name, sizeof name, "%dx%d", size.width, size.height);
    return name;
}

entity_t
render_lab_test_loaded_scene(const char* key) {
    for (int i = 0; i < SCENE_ROOTS_MAX; i++) {
        const scene_root_t* record = &scene_roots[i];
        if (!handle_is_none(record->root) && record->state == PENDING_READY && strcmp(record->id, key) == 0) {
            return record->root;
        }
    }
    return ENTITY_NONE;
}

void
render_lab_test_enter(render_lab_test_t* saved, const char* scene) {
    *saved = (render_lab_test_t){render_lab_start_scene_key, render_lab_start_camera, render_lab_show_hud};
    ui_init();
    render_lab_start_scene_key = scene;
    render_lab_start_camera = NULL;
    render_lab_show_hud = false;
    test_hold_quarter(DISPLAY_DEFAULT_QUARTER);
    /* Suites price whole draws, a still or paused one too; the partial A/B turns it on itself. */
    TEST_ASSERT_TRUE(tune_set(tune_shared(), TUNE_NAME(render, partial_redraw), 0));
    app_render_lab.enter();
}

void
render_lab_test_exit(const render_lab_test_t* saved) {
    if (gfx_present_guard_in_flight) {
        test_present_wait();
    }
    app_render_lab.exit();
    scene_unload_all();
    render_lab_start_scene_key = saved->scene;
    render_lab_start_camera = saved->camera;
    render_lab_show_hud = saved->hud;
    (void)tune_reset(tune_shared(), TUNE_NAME(render, partial_redraw));
    test_release_quarter();
}

int64_t
render_lab_test_frame_dt(uint32_t dt_ms, bool switching, const input_t* input) {
    TEST_ASSERT_TRUE_MESSAGE(test_quarter_held(), "a test frame needs a held quarter");
    static const input_t idle = {0};
    if (input == NULL) {
        input = &idle;
    }
    test_present_begin();
    app_render_lab.update(dt_ms, input);
    render_lab_apply_raster_settings();
    const int64_t render_start = timing_now_us();
    scene_shell_render(dt_ms);
    const int64_t render_us = timing_now_us() - render_start;
    test_present_wait();
    ui_clear_band_overlay();
    scene_shell_compose(dt_ms);
    if (switching) {
        TEST_ASSERT_TRUE_MESSAGE(gfx_frame_expanded(), "the switch must encounter an expanded picture");
    }
    app_render_lab.frame(dt_ms, input);
    if (switching) {
        TEST_ASSERT_FALSE_MESSAGE(gfx_frame_expanded(), "the new scene must discard the expanded picture");
        TEST_ASSERT_TRUE_MESSAGE(gfx_fb_guard_available, "the new scene must allow full-framebuffer writes");
        TEST_ASSERT_NOT_NULL(gfx_framebuffer());
    }
    test_present();
    return render_us;
}

void
render_lab_test_show_view(int view) {
    TEST_ASSERT_TRUE(tune_set(tune_shared(), TUNE_NAME(render_lab, view), view));
    render_context_set_debug_view(render_context_main(), view);
    TEST_ASSERT_EQUAL_INT(view, render_context_main()->debug_view);
}

/* A replayed pose's log line parts, too big for the stack its draws run on. */
#define POSE_COUNT_BYTES(name, member) char name[sizeof(" " #name "=-2147483648")];

typedef struct {
    char counts[sizeof(struct { RASTER_CULL_FRAME_FIELDS(POSE_COUNT_BYTES) char end; })];
    char report[FRAME_COST_REPORT_MAX];
} pose_line_t;

static pose_line_t* pose_line;

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

void
render_lab_test_replay_pose(const char* tag, entity_t entity, uint32_t duration_ms, int32_t path_ms, const char* row,
                            const char* quarter) {
    /* Static, so a failed assertion's jump leaves it for the next pose to reuse. */
    if (pose_line == NULL) {
        pose_line = memory_alloc(sizeof *pose_line, MEMORY_INTERNAL);
    }
    TEST_ASSERT_NOT_NULL(pose_line);
    pose_line_t* out = pose_line;
    TEST_ASSERT_TRUE(animator_seek(entity, ANIMATOR_CURRENT, (uint32_t)path_ms % duration_ms));
    render_lab_test_show_view(render_context_debug_view_named("cull"));
    (void)render_lab_test_frame_dt(0, false, NULL);
    write_cull_counts(out);
    render_lab_test_show_view(RENDER_DEBUG_VIEW_SHADED);
    (void)render_lab_test_frame_dt(0, false, NULL);
    (void)frame_cost_take_report(1, out->report, sizeof out->report);
    for (int i = 0; i < FRAME_PROFILE_REPLAY_DRAWS; i++) {
        (void)render_lab_test_frame_dt(0, false, NULL);
    }
    (void)frame_cost_take_report(FRAME_PROFILE_REPLAY_DRAWS, out->report, sizeof out->report);
    ESP_LOGI(tag, FRAME_PROFILE_REPLAY R3D_PERF_QUARTER_FORMAT, row, (int)path_ms, out->counts, out->report, quarter);
    memory_free(pose_line);
    pose_line = NULL;
}

#endif
