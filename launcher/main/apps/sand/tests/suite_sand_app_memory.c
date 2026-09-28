#include "suites.h"
#include "unity.h"

#ifdef DEVICE_BUILD

#include "app.h"
#include "app_memory.h"
#include "esp_heap_caps.h"

extern app_t app_sand;
extern int sand_app_enter_running_for_test(void);
extern void sand_app_restore_colour_mode_for_test(int mode);

static void
test_exit_returns_internal_memory(void) {
    const size_t free_before = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);

    app_sand.enter();
    const int previous_mode = sand_app_enter_running_for_test();
    app_sand.exit();
    sand_app_restore_colour_mode_for_test(previous_mode);

    const size_t free_after = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    TEST_ASSERT_EQUAL_UINT(0, app_internal_memory_leaked_bytes(free_before, free_after));
}

#endif /* DEVICE_BUILD */

void
run_sand_app_memory_suite(void) {
#ifdef DEVICE_BUILD
    RUN_TEST(test_exit_returns_internal_memory);
#endif
}

SUITE_REGISTER(run_sand_app_memory_suite);
