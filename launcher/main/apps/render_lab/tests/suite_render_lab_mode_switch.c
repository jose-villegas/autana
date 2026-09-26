#include "suites.h"
#include "unity.h"

#include "apps/render_lab/render_lab_mode_switch.h"

static void
test_a_requested_switch_is_taken_once(void) {
    render_lab_mode_switch_t state = {0};

    render_lab_mode_switch_request(&state);

    TEST_ASSERT_TRUE(render_lab_mode_switch_take(&state));
    TEST_ASSERT_FALSE(render_lab_mode_switch_take(&state));
}

static void
test_double_buffer_is_not_available_while_bands_are_active(void) {
    TEST_ASSERT_FALSE(render_lab_mode_switch_double_buffer_allowed(true, true));
    TEST_ASSERT_FALSE(render_lab_mode_switch_double_buffer_allowed(true, false));
    TEST_ASSERT_FALSE(render_lab_mode_switch_double_buffer_allowed(false, false));
    TEST_ASSERT_TRUE(render_lab_mode_switch_double_buffer_allowed(false, true));
}

void
run_render_lab_mode_switch_suite(void) {
    RUN_TEST(test_a_requested_switch_is_taken_once);
    RUN_TEST(test_double_buffer_is_not_available_while_bands_are_active);
}

SUITE_REGISTER(run_render_lab_mode_switch_suite);
