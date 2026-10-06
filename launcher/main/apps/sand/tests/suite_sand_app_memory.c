#include "suites.h"
#include "unity.h"

#ifdef DEVICE_BUILD

#include "app/app.h"
#include "apps/sand/sand_limits.h"
#include "util/memory.h"

extern app_t app_sand;
extern int sand_app_enter_running_for_test(void);
extern void sand_app_restore_colour_mode_for_test(int mode);

static void
test_running_then_reentering_returns_internal_memory(void) {
    app_sand.enter();
    const int warmup_mode = sand_app_enter_running_for_test();
    app_sand.exit();
    sand_app_restore_colour_mode_for_test(warmup_mode);

    const size_t internal_before = memory_free_bytes(MEMORY_INTERNAL);
    const size_t eight_bit_before = memory_free_bytes(MEMORY_8BIT);

    app_sand.enter();
    const int previous_mode = sand_app_enter_running_for_test();
    const size_t eight_bit_while_running = memory_free_bytes(MEMORY_8BIT);
    TEST_ASSERT_GREATER_OR_EQUAL_UINT((size_t)GRID_W_MAX * GRID_H_MAX, eight_bit_before - eight_bit_while_running);
    app_sand.exit();
    sand_app_restore_colour_mode_for_test(previous_mode);

    app_sand.enter();
    app_sand.invalidate();
    app_sand.exit();

    const size_t internal_after = memory_free_bytes(MEMORY_INTERNAL);
    const size_t eight_bit_after = memory_free_bytes(MEMORY_8BIT);
    TEST_ASSERT_EQUAL_UINT(internal_before, internal_after);
    TEST_ASSERT_EQUAL_UINT(eight_bit_before, eight_bit_after);
}

#endif /* DEVICE_BUILD */

void
run_sand_app_memory_suite(void) {
#ifdef DEVICE_BUILD
    RUN_TEST(test_running_then_reentering_returns_internal_memory);
#endif
}

SUITE_REGISTER(run_sand_app_memory_suite);
