#include "suites.h"
#include "unity.h"

#include "cube_mode_switch.h"

static void
test_a_requested_switch_is_taken_once(void) {
    cube_mode_switch_t state = {0};

    cube_mode_switch_request(&state);

    TEST_ASSERT_TRUE(cube_mode_switch_take(&state));
    TEST_ASSERT_FALSE(cube_mode_switch_take(&state));
}

void
run_cube_mode_switch_suite(void) {
    RUN_TEST(test_a_requested_switch_is_taken_once);
}

SUITE_REGISTER(run_cube_mode_switch_suite);
