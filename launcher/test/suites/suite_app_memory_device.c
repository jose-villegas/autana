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
test_every_app_returns_heap_memory(void) {
    warm_apps();

    for (const app_t* app = app_list(); app != NULL; app = app->next) {
        const size_t internal_before = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
        const size_t eight_bit_before = heap_caps_get_free_size(MALLOC_CAP_8BIT);
        app->enter();
        app->exit();
        const size_t internal_after = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
        const size_t eight_bit_after = heap_caps_get_free_size(MALLOC_CAP_8BIT);
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
