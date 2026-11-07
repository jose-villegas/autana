#include "suites.h"

#ifdef DEVICE_BUILD

#include "app/app.h"
#include "app/app_arena.h"
#include "scene/scene_shell.h"
#include "unity.h"

#include "core/memory.h"

static void
warm_apps(void) {
    for (const app_t* app = app_list(); app != NULL; app = app->next) {
        app->enter();
        app->exit();
        scene_unload_all();
        app_arena_rewind(0);
    }
}

static void
test_every_app_returns_heap_memory(void) {
    warm_apps();

    for (const app_t* app = app_list(); app != NULL; app = app->next) {
        const size_t internal_before = memory_free_bytes(MEMORY_INTERNAL);
        const size_t eight_bit_before = memory_free_bytes(MEMORY_8BIT);
        app->enter();
        app->exit();
        scene_unload_all();
        app_arena_rewind(0);
        const size_t internal_after = memory_free_bytes(MEMORY_INTERNAL);
        const size_t eight_bit_after = memory_free_bytes(MEMORY_8BIT);
        TEST_ASSERT_EQUAL_UINT_MESSAGE(internal_before, internal_after, app->name);
        TEST_ASSERT_EQUAL_UINT_MESSAGE(eight_bit_before, eight_bit_after, app->name);
    }
}

#endif

void
run_app_memory_device_suite(void) {
#ifdef DEVICE_BUILD
    RUN_TEST(test_every_app_returns_heap_memory);
#endif
}

SUITE_REGISTER(run_app_memory_device_suite);
