/* Device suite: clip dispatch and presentation cleanup across scene lifetimes. */
#include "suites.h"
#ifdef DEVICE_BUILD
#include <stdio.h>
#include "apps/render_lab/render_lab_clips.h"
#include "apps/render_lab/tests/render_lab_test.h"
#include "core/memory.h"
#include "display/display.h"
#include "gfx/present/gfx_present.h"
#include "gfx/present/gfx_present_guard.h"
#include "scene/scene_internal.h"
#include "test_cleanup.h"
#include "ui/layout/ui_transform.h"
#include "ui/ui.h"
#include "ui_probe.h"
#include "unity.h"

#define DISPATCH_BLEND_MS 300
#define DISPATCH_REPEATS  3

typedef struct {
    render_lab_test_t saved;
    bool entered;
} dispatch_fixture_t;

static dispatch_fixture_t* active;

static void
release_fixture(void) {
    if (active->entered) {
        render_lab_test_exit(&active->saved);
    }
    memory_free(active);
    active = NULL;
}

static dispatch_fixture_t*
fixture(void) {
    active = memory_alloc(sizeof *active, MEMORY_PSRAM);
    TEST_ASSERT_NOT_NULL(active);
    *active = (dispatch_fixture_t){0};
    suite_set_test_cleanup(release_fixture);
    return active;
}

static void
enter(dispatch_fixture_t* f, const char* key) {
    render_lab_test_enter(&f->saved, key);
    f->entered = true;
}

static void
leave(dispatch_fixture_t* f) {
    render_lab_test_exit(&f->saved);
    f->entered = false;
}

static void
test_scene_console_dispatch_and_repeated_exit_release_heap(void) {
    dispatch_fixture_t* f = fixture();
    enter(f, "capybara");
    leave(f);
    const size_t internal = memory_free_bytes(MEMORY_INTERNAL);
    const size_t psram = memory_free_bytes(MEMORY_PSRAM);
    for (int repeat = 0; repeat < DISPATCH_REPEATS; repeat++) {
        enter(f, "capybara");
        scene_t* scene = render_lab_test_loaded_scene("capybara");
        TEST_ASSERT_NOT_NULL(scene);
        const scene_entity_t subject = scene_find(scene, "capybara");
        TEST_ASSERT_NOT_EQUAL(SCENE_ENTITY_NONE, subject);
        const scene_animator_t* animator = &scene->animators[subject];
        const char* playing = scene_animator_clip_name(scene, subject, animator->layer.current.clip);
        TEST_ASSERT_EQUAL_STRING("idle", playing);
        TEST_ASSERT_TRUE(app_render_lab.console->handle("clips"));
        char command[ASSET_NAME_MAX + sizeof("clip  ") + RENDER_LAB_BLEND_TEXT_SIZE];
        TEST_ASSERT_TRUE(snprintf(command, sizeof command, "clip gallop %d", DISPATCH_BLEND_MS) >= 0);
        TEST_ASSERT_TRUE(app_render_lab.console->handle(command));
        TEST_ASSERT_EQUAL_STRING("gallop", scene_animator_clip_name(scene, subject, animator->layer.current.clip));
        TEST_ASSERT_EQUAL_UINT32(DISPATCH_BLEND_MS, animator->layer.blend_ms);
        leave(f);
        TEST_ASSERT_EQUAL_UINT(internal, memory_free_bytes(MEMORY_INTERNAL));
        TEST_ASSERT_EQUAL_UINT(psram, memory_free_bytes(MEMORY_PSRAM));
    }
    enter(f, "sponza");
    scene_t* plain = render_lab_test_loaded_scene("sponza");
    TEST_ASSERT_NOT_NULL(plain);
    const int animators = plain->animator_count;
    const size_t free_before = memory_free_bytes(MEMORY_PSRAM);
    TEST_ASSERT_FALSE(app_render_lab.console->handle("clip gallop 0"));
    TEST_ASSERT_FALSE(app_render_lab.console->handle("clips"));
    TEST_ASSERT_EQUAL_INT(animators, plain->animator_count);
    TEST_ASSERT_EQUAL_UINT(free_before, memory_free_bytes(MEMORY_PSRAM));
}

static void
test_boot_menu_draws_controls_only_for_animated_scene(void) {
    dispatch_fixture_t* f = fixture();
    static const char* const keys[] = {"capybara", "sponza"};
    for (size_t i = 0; i < sizeof keys / sizeof keys[0]; i++) {
        enter(f, keys[i]);
        render_lab_test_frame(false, NULL);
        const input_t boot = {.boot = {.pressed = true}};
        render_lab_test_frame(false, &boot);
        render_lab_test_frame(false, NULL);
        char blend[sizeof("BLEND:  ms") + RENDER_LAB_BLEND_TEXT_SIZE];
        TEST_ASSERT_TRUE(snprintf(blend, sizeof blend, "BLEND: %d ms", RENDER_LAB_BLEND_DEFAULT_MS) >= 0);
        recti_t drawn;
        TEST_ASSERT_TRUE(ui_probe_text("BOOT to close", &drawn));
        TEST_ASSERT_EQUAL_INT(i == 0, ui_probe_text("CLIP", &drawn));
        TEST_ASSERT_EQUAL_INT(i == 0, ui_probe_text(blend, &drawn));
        leave(f);
    }
}

static void
test_boot_clip_opening_touch_does_not_select_overlay_row(void) {
    dispatch_fixture_t* f = fixture();

    enum { HOLD_FRAMES = 8, MOTION_PIXELS = 2, OVERLAPPED_ROW = 2 };

    for (int motion = 0; motion <= MOTION_PIXELS; motion += MOTION_PIXELS) {
        enter(f, "capybara");
        /* The fixture resets the transform; the shell supplies landscape on the board. */
        ui_set_transform(ui_transform_quarter_turn(DISPLAY_LANDSCAPE, GFX_WIDTH, GFX_HEIGHT));
        TEST_ASSERT_EQUAL_INT(GFX_HEIGHT, ui_width());
        TEST_ASSERT_EQUAL_INT(GFX_WIDTH, ui_height());
        render_lab_test_frame(false, NULL);
        const input_t boot = {.boot = {.pressed = true}};
        render_lab_test_frame(false, &boot);
        render_lab_test_frame(false, NULL);
        input_t touch = {.down = true, .pressed = true};
        recti_t idle;
        TEST_ASSERT_TRUE(ui_probe_text("idle", &idle));
        const affine2x_t transform = ui_effective_transform();
        const vec2i_t mapped_pixel = affine2x_map_pixel(&transform, (vec2i_t){idle.x, idle.y});
        touch.x = mapped_pixel.x;
        touch.y = mapped_pixel.y;
        render_lab_test_frame(false, &touch);
        touch.pressed = false;
        for (int frame = 0; frame < HOLD_FRAMES; frame++) {
            touch.y += frame < motion;
            render_lab_test_frame(false, &touch);
        }
        touch.down = false;
        touch.released = true;
        render_lab_test_frame(false, &touch);
        recti_t rows;
        int scroll;
        TEST_ASSERT_TRUE(ui_probe_list(&rows, &scroll));
        TEST_ASSERT_EQUAL_INT(UI_MARGIN, rows.y);
        TEST_ASSERT_TRUE(idle.x >= rows.x && idle.x < recti_right(rows));
        const int covered_row = (idle.y - rows.y + scroll) / UI_ROW_HEIGHT;
        TEST_ASSERT_EQUAL_INT(OVERLAPPED_ROW, covered_row);
        scene_t* scene = render_lab_test_loaded_scene("capybara");
        const scene_entity_t subject = scene_find(scene, "capybara");
        const scene_animator_t* animator = &scene->animators[subject];
        TEST_ASSERT_EQUAL_STRING_MESSAGE("idle", scene_animator_clip_name(scene, subject, animator->layer.current.clip),
                                         "opening release must not play the overlay row under the finger");
        for (int frame = 0; frame < UI_DROPDOWN_CLOSE_FRAMES; frame++) {
            render_lab_test_frame(false, NULL);
            TEST_ASSERT_TRUE(ui_probe_list(&rows, &scroll));
        }
        const int item = 1;
        const char* expected = scene_animator_clip_name(scene, subject, item);
        touch = (input_t){.down = true, .pressed = true};
        const vec2i_t item_pixel =
            affine2x_map_pixel(&transform, (vec2i_t){rows.x + (rows.w / 2),
                                                     rows.y + (item * UI_ROW_HEIGHT) + (UI_ROW_HEIGHT / 2) - scroll});
        touch.x = item_pixel.x;
        touch.y = item_pixel.y;
        render_lab_test_frame(false, &touch);
        touch.pressed = false;
        for (int frame = 0; frame < HOLD_FRAMES; frame++) {
            render_lab_test_frame(false, &touch);
        }
        touch.down = false;
        touch.released = true;
        render_lab_test_frame(false, &touch);
        TEST_ASSERT_EQUAL_STRING(expected, scene_animator_clip_name(scene, subject, animator->layer.current.clip));
        leave(f);
    }
}

static void
test_cleanup_finishes_present_without_tripping_guard(void) {
    dispatch_fixture_t* f = fixture();
    enter(f, "capybara");
    const unsigned trips = gfx_present_guard_trips;
    gfx_present_begin();
    TEST_ASSERT_TRUE(gfx_present_guard_in_flight);
    leave(f);
    TEST_ASSERT_FALSE(gfx_present_guard_in_flight);
    TEST_ASSERT_EQUAL_UINT(trips, gfx_present_guard_trips);
}
#endif

void
run_clip_dispatch_suite(void) {
#ifdef DEVICE_BUILD
    RUN_TEST(test_boot_clip_opening_touch_does_not_select_overlay_row);
    RUN_TEST(test_boot_menu_draws_controls_only_for_animated_scene);
    RUN_TEST(test_scene_console_dispatch_and_repeated_exit_release_heap);
    RUN_TEST(test_cleanup_finishes_present_without_tripping_guard);
#endif
}

SUITE_REGISTER(run_clip_dispatch_suite);
