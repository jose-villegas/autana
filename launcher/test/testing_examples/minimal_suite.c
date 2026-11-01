/* A real button suite feeds samples with an explicit clock. */
#include "suites.h"
#include "unity.h"

#include "input/buttons/button_fsm.h"

static button_fsm_t b;
static int64_t now;

static void
fixture(void) {
    button_fsm_reset(&b);
    now = BUTTON_DEBOUNCE_US;
}

static void
test_a_held_button_press_is_reported_once(void) {
    fixture();
    button_fsm_update(&b, false, now);
    button_fsm_update(&b, true, ++now);
    now += BUTTON_DEBOUNCE_US;
    button_fsm_update(&b, true, now);
    TEST_ASSERT_TRUE_MESSAGE(button_fsm_take_pressed(&b), "a level held past the debounce window must produce a press");
    button_fsm_update(&b, true, ++now);
    TEST_ASSERT_FALSE_MESSAGE(button_fsm_take_pressed(&b), "an edge must be consumed by the frame that reads it");
}

void
run_minimal_example_suite(void) {
    RUN_TEST(test_a_held_button_press_is_reported_once);
}

SUITE_REGISTER(run_minimal_example_suite);
