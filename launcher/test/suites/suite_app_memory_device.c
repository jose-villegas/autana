#include "suites.h"

#ifdef DEVICE_BUILD

#include "app.h"
#include "esp_heap_caps.h"
#include "unity.h"

static void
warm_apps(void) {
    for (const app_t* app = app_list(); app != NULL; app = app->next) {
        app->enter();
        app->exit();
    }
}

static void
test_every_app_returns_internal_memory(void) {
    warm_apps();

    for (const app_t* app = app_list(); app != NULL; app = app->next) {
        const size_t free_before = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
        app->enter();
        app->exit();
        const size_t free_after = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
        TEST_ASSERT_EQUAL_UINT_MESSAGE(free_before, free_after, app->name);
    }
}

#endif

void
run_app_memory_device_suite(void) {
#ifdef DEVICE_BUILD
    RUN_TEST(test_every_app_returns_internal_memory);
#endif
}

SUITE_REGISTER(run_app_memory_device_suite);
