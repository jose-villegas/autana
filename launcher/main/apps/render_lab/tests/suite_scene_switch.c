/* Device suite: scene requests applied after the shell composes an expanded frame. */
#include "suites.h"

#ifdef DEVICE_BUILD

#include "test_cleanup.h"
#include "unity.h"

#include "app/app.h"
#include "apps/render_lab/render_lab.h"
#include "gfx/present/gfx_fb_guard.h"
#include "gfx/present/gfx_mode.h"
#include "gfx/present/gfx_present.h"
#include "render/context/render_context.h"
#include "scene/scene_shell.h"
#include "ui/ui.h"

extern app_t app_render_lab;

#define FRAME_DT_MS        16
#define HALF_SCALE_PERCENT 50

static const char* saved_scene;
static const char* saved_camera;
static bool saved_hud;

static void
release_fixture(void) {
    gfx_present_wait();
    app_render_lab.exit();
    scene_unload_all();
    render_lab_start_scene_key = saved_scene;
    render_lab_start_camera = saved_camera;
    render_lab_show_hud = saved_hud;
}

static void
fixture(void) {
    saved_scene = render_lab_start_scene_key;
    saved_camera = render_lab_start_camera;
    saved_hud = render_lab_show_hud;
    ui_init();
    render_lab_start_scene_key = "sponza";
    render_lab_start_camera = NULL;
    render_lab_show_hud = false;
    app_render_lab.enter();
    suite_set_test_cleanup(release_fixture);
    TEST_ASSERT_TRUE(scene_has_active_camera());
    TEST_ASSERT_NOT_NULL(app_render_lab.console);
}

/* The switch runs after composition, before this frame is presented. */
static void
run_frame(bool switching) {
    const input_t idle = {0};
    gfx_present_begin();
    app_render_lab.update(FRAME_DT_MS, &idle);
    render_context_set_dynamic_resolution(render_context_main(), NULL, NULL, 0);
    render_context_set_scale(render_context_main(), HALF_SCALE_PERCENT);
    scene_shell_render(FRAME_DT_MS);
    gfx_present_wait();
    ui_clear_band_overlay();
    scene_shell_compose(FRAME_DT_MS);
    TEST_ASSERT_TRUE_MESSAGE(gfx_frame_expanded(), "the switch must encounter an expanded picture");
    app_render_lab.frame(FRAME_DT_MS, &idle);
    if (switching) {
        TEST_ASSERT_FALSE_MESSAGE(gfx_frame_expanded(), "the new scene must discard the expanded picture");
        TEST_ASSERT_TRUE_MESSAGE(gfx_fb_guard_available, "the new scene must allow full-framebuffer writes");
        TEST_ASSERT_NOT_NULL(gfx_framebuffer());
    }
    gfx_present();
}

static void
test_console_scene_requests_survive_expanded_frames(void) {
    fixture();

    static const struct {
        const char* request;
        const char* key;
    } switches[] = {
        {"scene sponza-lite", "sponza-lite"}, {"scene sponza", "sponza"}, {"scene sponza-flat", "sponza-flat"},
        {"scene sponza-lite", "sponza-lite"}, {"scene sponza", "sponza"},
    };

    run_frame(false);
    for (size_t i = 0; i < sizeof switches / sizeof switches[0]; i++) {
        TEST_ASSERT_TRUE(app_render_lab.console->handle(switches[i].request));
        run_frame(true);
        TEST_ASSERT_EQUAL_STRING(switches[i].key, render_lab_start_scene_key);
        TEST_ASSERT_TRUE(scene_has_active_camera());
        run_frame(false);
    }
}

#endif

void
run_render_lab_scene_switch_suite(void) {
#ifdef DEVICE_BUILD
    RUN_TEST(test_console_scene_requests_survive_expanded_frames);
#endif
}

SUITE_REGISTER(run_render_lab_scene_switch_suite);
