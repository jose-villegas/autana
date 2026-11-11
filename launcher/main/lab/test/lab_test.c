#include "lab/test/lab_test.h"
#ifdef DEVICE_BUILD
#include <stdio.h>
#include <string.h>
#include "core/timing.h"
#include "core/tune.h"
#include "frame/test_frame.h"
#include "gfx_shared_state.h"
#include "scene/scene_internal.h"
#include "scene/scene_shell.h"
#include "ui/ui.h"
#include "unity.h"

entity_t
lab_test_loaded_scene(const char* key) {
    for (int i = 0; i < SCENE_ROOTS_MAX; i++) {
        const scene_root_t* record = &scene_roots[i];
        if (!handle_is_none(record->root) && record->state == PENDING_READY && strcmp(record->id, key) == 0) {
            return record->root;
        }
    }
    return ENTITY_NONE;
}

/* The lab the frames below drive, from lab_test_enter() to lab_test_exit(). */
static lab_t* entered;

void
lab_test_enter(lab_test_t* saved, lab_t* lab, const char* scene) {
    *saved = (lab_test_t){lab, lab->start_key, lab->start_camera, lab->show_hud};
    entered = lab;
    ui_init();
    lab->start_key = scene;
    lab->start_camera = NULL;
    lab->show_hud = false;
    test_hold_quarter(DISPLAY_DEFAULT_QUARTER);
    /* Suites price whole draws, a still or paused one too; the partial A/B turns it on itself. */
    TEST_ASSERT_TRUE(tune_set(tune_shared(), TUNE_NAME(render, partial_redraw), 0));
    lab->app->enter();
}

void
lab_test_exit(const lab_test_t* saved) {
    if (gfx_present_guard_in_flight) {
        test_present_wait();
    }
    saved->lab->app->exit();
    scene_unload_all();
    saved->lab->start_key = saved->scene;
    saved->lab->start_camera = saved->camera;
    saved->lab->show_hud = saved->hud;
    entered = NULL;
    (void)tune_reset(tune_shared(), TUNE_NAME(render, partial_redraw));
    test_release_quarter();
}

int64_t
lab_test_frame_dt(uint32_t dt_ms, bool switching, const input_t* input) {
    TEST_ASSERT_TRUE_MESSAGE(test_quarter_held(), "a test frame needs a held quarter");
    static const input_t idle = {0};
    if (input == NULL) {
        input = &idle;
    }
    test_present_begin();
    entered->app->update(dt_ms, input);
    if (entered->hooks.test_render != NULL) {
        entered->hooks.test_render();
    }
    const int64_t render_start = timing_now_us();
    scene_shell_render(dt_ms);
    const int64_t render_us = timing_now_us() - render_start;
    test_present_wait();
    ui_clear_band_overlay();
    scene_shell_compose(dt_ms);
    if (switching) {
        TEST_ASSERT_TRUE_MESSAGE(gfx_frame_expanded(), "the switch must encounter an expanded picture");
    }
    entered->app->frame(dt_ms, input);
    if (switching) {
        TEST_ASSERT_FALSE_MESSAGE(gfx_frame_expanded(), "the new scene must discard the expanded picture");
        TEST_ASSERT_TRUE_MESSAGE(gfx_fb_guard_available, "the new scene must allow full-framebuffer writes");
        TEST_ASSERT_NOT_NULL(gfx_framebuffer());
    }
    test_present();
    return render_us;
}

#endif
