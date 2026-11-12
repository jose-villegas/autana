#include "suites.h"
#ifdef DEVICE_BUILD
#include "app/app_arena.h"
#include "apps/physics_lab/physics_lab.h"
#include "core/memory.h"
#include "draw/draw.h"
#include "frame/test_frame.h"
#include "image/pixel/rgb565.h"
#include "lab/test/lab_test.h"
#include "testkit/cleanup.h"
#include "ui/ui.h"
#include "unity.h"

#define PHYSICS_TEST_FRAMES   3
#define PHYSICS_TEST_SENTINEL 0xFFFFFF
#define PHYSICS_STAGE_COMMAND "scene " PHYSICS_STAGE_KEY
#define PHYSICS_FB_PIXELS     (GFX_FRAMEBUFFER_BYTES / sizeof(gfx_color_t))

static lab_test_t saved;
static bool entered;

static void
release_fixture(void) {
    if (entered) {
        lab_test_exit(&saved);
        entered = false;
    }
}

static void
fixture(void) {
    entered = false;
    suite_set_test_cleanup(release_fixture);
}

static void
enter(void) {
    entered = true;
    lab_test_enter(&saved, &physics_lab, PHYSICS_STAGE_KEY);
}

static void
leave(size_t arena, size_t internal, size_t heap) {
    release_fixture();
    TEST_ASSERT_EQUAL_UINT(arena, app_arena_mark());
    TEST_ASSERT_EQUAL_UINT(internal, memory_free_bytes(MEMORY_INTERNAL));
    TEST_ASSERT_EQUAL_UINT(heap, memory_free_bytes(MEMORY_APP));
}

/* Pixels of the framebuffer that are not the UI background. */
static size_t
pixels_off_background(void) {
    const gfx_color_t* fb = gfx_framebuffer();
    size_t off = 0;
    for (size_t i = 0; i < PHYSICS_FB_PIXELS; i++) {
        off += fb[i] != RGB565BE(UI_BACKGROUND);
    }
    return off;
}

static void
test_stage_draws_background_and_releases_memory(void) {
    fixture();
    const size_t arena = app_arena_mark();
    const size_t internal = memory_free_bytes(MEMORY_INTERNAL);
    const size_t heap = memory_free_bytes(MEMORY_APP);
    draw_clear(RGB565BE(PHYSICS_TEST_SENTINEL));
    enter();
    TEST_ASSERT_EQUAL_PTR(&scene_physics_stage, lab_current(&physics_lab));
    test_send_stats_reset();
    for (int frame = 0; frame < PHYSICS_TEST_FRAMES; frame++) {
        (void)lab_test_frame(false, NULL);
        TEST_ASSERT_EQUAL_size_t(0, pixels_off_background());
    }
    TEST_ASSERT_GREATER_THAN_INT64(0, test_send_stats().bytes);
    leave(arena, internal, heap);
}

/* The BOOT menu draws over the stage; closing it leaves the bare stage. */
static void
test_closing_the_menu_restores_the_stage(void) {
    fixture();
    const input_t boot = {.boot = {.pressed = true}};
    enter();
    (void)lab_test_frame(false, NULL);
    (void)lab_test_frame(false, &boot);
    TEST_ASSERT_TRUE(lab_menu_open(&physics_lab));
    TEST_ASSERT_GREATER_THAN_size_t(0, pixels_off_background());
    (void)lab_test_frame(false, &boot);
    TEST_ASSERT_FALSE(lab_menu_open(&physics_lab));
    TEST_ASSERT_EQUAL_size_t(0, pixels_off_background());
    release_fixture();
}

static void
test_console_lists_stage_and_switches_without_leaking(void) {
    fixture();
    const size_t arena = app_arena_mark();
    const size_t internal = memory_free_bytes(MEMORY_INTERNAL);
    const size_t heap = memory_free_bytes(MEMORY_APP);
    enter();
    TEST_ASSERT_TRUE(physics_lab_console("scenes"));
    TEST_ASSERT_EQUAL_STRING(PHYSICS_STAGE_KEY, physics_lab.scenes[0]->key);
    for (int repeat = 0; repeat < PHYSICS_TEST_FRAMES; repeat++) {
        TEST_ASSERT_TRUE(physics_lab_console(PHYSICS_STAGE_COMMAND));
        (void)lab_test_frame(false, NULL);
        TEST_ASSERT_EQUAL_PTR(&scene_physics_stage, lab_current(&physics_lab));
        TEST_ASSERT_TRUE(handle_is_none(lab_test_loaded_scene(PHYSICS_STAGE_KEY)));
    }
    leave(arena, internal, heap);
}
#endif

void
run_physics_lab_suite(void) {
#ifdef DEVICE_BUILD
    RUN_TEST(test_stage_draws_background_and_releases_memory);
    RUN_TEST(test_closing_the_menu_restores_the_stage);
    RUN_TEST(test_console_lists_stage_and_switches_without_leaking);
#endif
}

SUITE_REGISTER(run_physics_lab_suite);
