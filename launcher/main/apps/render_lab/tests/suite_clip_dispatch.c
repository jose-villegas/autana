/* Device suite: clip dispatch and presentation cleanup across scene lifetimes. */
#include "suites.h"
#ifdef DEVICE_BUILD
#include <stdio.h>
#include <string.h>
#include "apps/render_lab/tests/render_lab_test.h"
#include "core/memory.h"
#include "gfx/present/gfx_present.h"
#include "gfx/present/gfx_present_guard.h"
#include "scene/scene_internal.h"
#include "test_cleanup.h"
#include "unity.h"

#define DISPATCH_BLEND_MS 300
#define DISPATCH_REPEATS  3

typedef struct {
    render_lab_test_t saved;
    bool entered;
    int current_markers;
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
capture_reply(const char* line) {
    if (strstr(line, "current=1") != NULL) {
        TEST_ASSERT_EQUAL_STRING("RENDER clip=idle current=1", line);
        active->current_markers++;
    }
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
        f->current_markers = 0;
        TEST_ASSERT_TRUE(scene_capybara_test_reply("clips", capture_reply));
        TEST_ASSERT_EQUAL_INT(1, f->current_markers);
        char command[ASSET_NAME_MAX + sizeof("clip  1000")];
        (void)snprintf(command, sizeof command, "clip gallop %d", DISPATCH_BLEND_MS);
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
test_exit_finishes_present_without_tripping_guard(void) {
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
    RUN_TEST(test_scene_console_dispatch_and_repeated_exit_release_heap);
    RUN_TEST(test_exit_finishes_present_without_tripping_guard);
#endif
}

SUITE_REGISTER(run_clip_dispatch_suite);
