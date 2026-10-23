/* Device suite: scene requests applied after the shell composes an expanded
 * frame, and what each scene takes from the app arena given back on a switch. */
#include "suites.h"

#ifdef DEVICE_BUILD

#include <stdio.h>

#include "test_cleanup.h"
#include "unity.h"

#include "app/app_arena.h"
#include "apps/render_lab/render_lab.h"
#include "apps/render_lab/render_lab_dynres.h"
#include "apps/render_lab/render_lab_scene.h"
#include "apps/render_lab/scene_baked.h"
#include "apps/render_lab/tests/render_lab_test.h"
#include "render/context/render_context.h"
#include "scene/scene_shell.h"

static render_lab_test_t saved;

static void
release_fixture(void) {
#if TUNE_ENABLED
    (void)render_lab_dynres_set_on(false);
#endif
    render_lab_test_exit(&saved);
}

static void
fixture(void) {
    render_lab_test_enter(&saved, "sponza");
    suite_set_test_cleanup(release_fixture);
    TEST_ASSERT_TRUE(scene_has_active_camera());
    TEST_ASSERT_NOT_NULL(app_render_lab.console);
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

    render_lab_test_frame(false, NULL);
    TEST_ASSERT_FALSE(app_render_lab.console->handle("clips"));
    TEST_ASSERT_FALSE(app_render_lab.console->handle("clip gallop"));
    TEST_ASSERT_FALSE(app_render_lab.console->handle("unknown"));
    for (size_t i = 0; i < sizeof switches / sizeof switches[0]; i++) {
        TEST_ASSERT_TRUE(app_render_lab.console->handle(switches[i].request));
        render_lab_test_frame(true, NULL);
        TEST_ASSERT_EQUAL_STRING(switches[i].key, render_lab_start_scene_key);
        TEST_ASSERT_TRUE(scene_has_active_camera());
        render_lab_test_frame(false, NULL);
    }
}

/* Shows `key` through the console, as a menu tap would, and draws it twice. */
static void
show_scene(const char* key) {
    char request[RENDER_LAB_STATUS_LEN];
    TEST_ASSERT_GREATER_THAN_INT(0, snprintf(request, sizeof request, "scene %s", key));
    TEST_ASSERT_TRUE_MESSAGE(app_render_lab.console->handle(request), key);
    render_lab_test_frame(false, NULL);
    render_lab_test_frame(false, NULL);
    TEST_ASSERT_EQUAL_STRING(key, render_lab_start_scene_key);
}

/* Every scene, lap after lap: a switch gives back all the leaving scene took
 * from the app arena, its frame scratch included, so each lap ends with the
 * arena where the first ended, and leaving puts it back where entering found it. */
static void
test_scene_switches_keep_the_arena_flat(void) {
    enum { LAPS = 3 };

    const size_t before = app_arena_mark();
    fixture();
    size_t first_lap = 0;
    for (int lap = 0; lap < LAPS; lap++) {
        for (int v = 0; v < (int)SCENE_BAKED_VARIANT_COUNT; v++) {
            show_scene(scene_sponza[v].key);
            show_scene(scene_bistro[v].key);
        }
        show_scene(scene_capybara.key);
        show_scene("sponza");
        const size_t lap_end = app_arena_mark();
        TEST_ASSERT_GREATER_THAN_size_t(before, lap_end); /* the showing scene lives in the arena */
        if (lap == 0) {
            first_lap = lap_end;
        }
        TEST_ASSERT_EQUAL_size_t(first_lap, lap_end);
    }
    suite_clear_test_cleanup();
    release_fixture();
    TEST_ASSERT_EQUAL_size_t(before, app_arena_mark());
}

#if TUNE_ENABLED
/* The menu's toggle holds across a switch to a scene of another family: a
 * frame under it draws at a ladder step, and off returns to the fixed scale. */
static void
test_dynamic_resolution_toggle_follows_the_showing_scene(void) {
    fixture();
    render_lab_test_frame(false, NULL);
    TEST_ASSERT_LESS_THAN_INT(0, render_context_frame(render_context_main()).step);

    TEST_ASSERT_TRUE(render_lab_dynres_set_on(true));
    render_lab_test_frame(false, NULL);
    render_lab_test_frame(false, NULL);
    TEST_ASSERT_GREATER_OR_EQUAL_INT(0, render_context_frame(render_context_main()).step);

    /* The step the budget picks decides whether this frame is a half
     * picture, so the switch need not meet an expanded one; the suite's
     * fixed-scale test holds that path. */
    TEST_ASSERT_TRUE(app_render_lab.console->handle("scene capybara"));
    render_lab_test_frame(false, NULL);
    render_lab_test_frame(false, NULL);
    TEST_ASSERT_EQUAL_STRING("capybara", render_lab_start_scene_key);
    TEST_ASSERT_GREATER_OR_EQUAL_INT(0, render_context_frame(render_context_main()).step);

    TEST_ASSERT_TRUE(render_lab_dynres_set_on(false));
    render_lab_test_frame(false, NULL);
    render_lab_test_frame(false, NULL);
    TEST_ASSERT_LESS_THAN_INT(0, render_context_frame(render_context_main()).step);
}
#endif

#endif

void
run_render_lab_scene_switch_suite(void) {
#ifdef DEVICE_BUILD
    RUN_TEST(test_console_scene_requests_survive_expanded_frames);
    RUN_TEST(test_scene_switches_keep_the_arena_flat);
#if TUNE_ENABLED
    RUN_TEST(test_dynamic_resolution_toggle_follows_the_showing_scene);
#endif
#endif
}

SUITE_REGISTER(run_render_lab_scene_switch_suite);
