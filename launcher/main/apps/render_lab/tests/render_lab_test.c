#include "apps/render_lab/tests/render_lab_test.h"
#ifdef DEVICE_BUILD
#include <string.h>
#include "apps/render_lab/render_lab.h"
#include "apps/render_lab/render_lab_view.h"
#include "core/timing.h"
#include "gfx/present/gfx_fb_guard.h"
#include "gfx/present/gfx_mode.h"
#include "gfx/present/gfx_present.h"
#include "gfx/present/gfx_present_guard.h"
#include "render/context/render_context.h"
#include "scene/scene_internal.h"
#include "scene/scene_shell.h"
#include "ui/ui.h"
#include "unity.h"

scene_t*
render_lab_test_loaded_scene(const char* key) {
    for (int i = 0; i < scene_loaded_count(); i++) {
        scene_t* scene = scene_loaded_at(i);
        if (strcmp(scene->id, key) == 0) {
            return scene;
        }
    }
    return NULL;
}

void
render_lab_test_enter(render_lab_test_t* saved, const char* scene) {
    *saved = (render_lab_test_t){render_lab_start_scene_key, render_lab_start_camera, render_lab_show_hud};
    ui_init();
    render_lab_start_scene_key = scene;
    render_lab_start_camera = NULL;
    render_lab_show_hud = false;
    app_render_lab.enter();
}

void
render_lab_test_exit(const render_lab_test_t* saved) {
    if (gfx_present_guard_in_flight) {
        gfx_present_wait();
    }
    app_render_lab.exit();
    scene_unload_all();
    render_lab_start_scene_key = saved->scene;
    render_lab_start_camera = saved->camera;
    render_lab_show_hud = saved->hud;
}

int64_t
render_lab_test_frame(bool switching, const input_t* input) {
    const input_t idle = {0};
    if (input == NULL) {
        input = &idle;
    }
    gfx_present_begin();
    app_render_lab.update(RENDER_LAB_TEST_FRAME_MS, input);
    render_context_set_dynamic_resolution(render_context_main(), NULL, NULL, 0);
    render_lab_apply_raster_settings();
    const int64_t render_start = timing_now_us();
    scene_shell_render(RENDER_LAB_TEST_FRAME_MS);
    const int64_t render_us = timing_now_us() - render_start;
    gfx_present_wait();
    ui_clear_band_overlay();
    scene_shell_compose(RENDER_LAB_TEST_FRAME_MS);
    if (switching) {
        TEST_ASSERT_TRUE_MESSAGE(gfx_frame_expanded(), "the switch must encounter an expanded picture");
    }
    app_render_lab.frame(RENDER_LAB_TEST_FRAME_MS, input);
    if (switching) {
        TEST_ASSERT_FALSE_MESSAGE(gfx_frame_expanded(), "the new scene must discard the expanded picture");
        TEST_ASSERT_TRUE_MESSAGE(gfx_fb_guard_available, "the new scene must allow full-framebuffer writes");
        TEST_ASSERT_NOT_NULL(gfx_framebuffer());
    }
    gfx_present();
    return render_us;
}

#endif
