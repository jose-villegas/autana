#include "apps/input_lab/guided_step.h"
#include "apps/input_lab/orientation_probe.h"
#include "display/display_judge.h"
#include "suites.h"
#include "unity.h"

#define GUIDE_TEST_START_US    7000
#define GUIDE_TEST_DURATION_US ((int64_t)ORIENTATION_PROBE_STEP_MS * MATH_US_PER_MS)

static guided_step_t
fixture(void) {
    guided_step_t guide;
    guided_step_begin(&guide, ORIENTATION_PROBE_READ_0, GUIDE_TEST_START_US);
    return guide;
}

static void
test_ready_waits_and_tap_restarts_clock(void) {
    guided_step_t g = fixture();
    const int64_t tap_us = GUIDE_TEST_START_US + GUIDE_TEST_DURATION_US;
    TEST_ASSERT_EQUAL_INT(GUIDED_STEP_WAIT, guided_step_update(&g, false, tap_us, GUIDE_TEST_DURATION_US));
    TEST_ASSERT_EQUAL_INT64(GUIDE_TEST_DURATION_US, guided_step_remaining_us(&g, tap_us, GUIDE_TEST_DURATION_US));
    TEST_ASSERT_EQUAL_INT(GUIDED_STEP_ARMED, guided_step_update(&g, true, tap_us, GUIDE_TEST_DURATION_US));
    TEST_ASSERT_FALSE(g.ready);
    TEST_ASSERT_EQUAL_INT64(tap_us, g.phase_us);
    TEST_ASSERT_EQUAL_INT64(GUIDE_TEST_DURATION_US, guided_step_remaining_us(&g, tap_us, GUIDE_TEST_DURATION_US));
}

static void
test_exact_deadline_advances_and_next_needs_own_tap(void) {
    guided_step_t g = fixture();
    guided_step_update(&g, true, GUIDE_TEST_START_US, GUIDE_TEST_DURATION_US);
    const int64_t end_us = GUIDE_TEST_START_US + GUIDE_TEST_DURATION_US;
    TEST_ASSERT_EQUAL_INT64(1, guided_step_remaining_us(&g, end_us - 1, GUIDE_TEST_DURATION_US));
    TEST_ASSERT_EQUAL_INT(GUIDED_STEP_WAIT, guided_step_update(&g, true, end_us - 1, GUIDE_TEST_DURATION_US));
    TEST_ASSERT_EQUAL_INT64(0, guided_step_remaining_us(&g, end_us, GUIDE_TEST_DURATION_US));
    TEST_ASSERT_EQUAL_INT64(0, guided_step_remaining_us(&g, end_us + 1, GUIDE_TEST_DURATION_US));
    TEST_ASSERT_EQUAL_INT(GUIDED_STEP_NEXT, guided_step_update(&g, true, end_us, GUIDE_TEST_DURATION_US));
    TEST_ASSERT_EQUAL_INT(ORIENTATION_PROBE_READ_1, g.step);
    TEST_ASSERT_TRUE(g.ready);
    TEST_ASSERT_EQUAL_INT(GUIDED_STEP_WAIT,
                          guided_step_update(&g, false, end_us + GUIDE_TEST_DURATION_US, GUIDE_TEST_DURATION_US));
    TEST_ASSERT_EQUAL_INT(ORIENTATION_PROBE_READ_1, g.step);
    TEST_ASSERT_EQUAL_INT(GUIDED_STEP_ARMED,
                          guided_step_update(&g, true, end_us + GUIDE_TEST_DURATION_US, GUIDE_TEST_DURATION_US));
}

static void
test_orientation_actions_bearings_and_invalid_prompts(void) {
    static const char* const actions[] = {"HOLD",    "TURN TO", "TURN TO", "TURN TO",  "TURN TO", "PUT DOWN FLAT",
                                          "PICK UP", "WALK",    "SHAKE",   "OVERHEAD", ""};
    for (int step = ORIENTATION_PROBE_READ_0; step <= ORIENTATION_PROBE_DONE; step++) {
        TEST_ASSERT_EQUAL_STRING(actions[step], orientation_probe_action((orientation_probe_step_t)step));
    }
    TEST_ASSERT_EQUAL_INT(0, orientation_probe_bearing(DISPLAY_LANDSCAPE_UPSIDE_DOWN));
    TEST_ASSERT_EQUAL_INT((int)DISPLAY_QUARTER_DEG, orientation_probe_bearing(DISPLAY_PORTRAIT));
    TEST_ASSERT_EQUAL_INT((int)DISPLAY_QUARTER_DEG * 2, orientation_probe_bearing(DISPLAY_LANDSCAPE));
    TEST_ASSERT_EQUAL_INT((int)DISPLAY_QUARTER_DEG * 3, orientation_probe_bearing(DISPLAY_PORTRAIT_UPSIDE_DOWN));
    char prompt[ORIENTATION_PROBE_PROMPT_SIZE];
    orientation_probe_prompt((orientation_probe_step_t)-1, prompt, sizeof prompt);
    TEST_ASSERT_EQUAL_STRING("", prompt);
    orientation_probe_prompt(ORIENTATION_PROBE_DONE, prompt, sizeof prompt);
    TEST_ASSERT_EQUAL_STRING("DONE: LABDUMP TO COLLECT", prompt);
    TEST_ASSERT_EQUAL_INT(DISPLAY_QUARTER_NONE, orientation_probe_target((orientation_probe_step_t)-1));
    TEST_ASSERT_EQUAL_INT(DISPLAY_QUARTER_NONE, orientation_probe_target(ORIENTATION_PROBE_DONE));
}

void
run_guided_step_suite(void) {
    RUN_TEST(test_ready_waits_and_tap_restarts_clock);
    RUN_TEST(test_exact_deadline_advances_and_next_needs_own_tap);
    RUN_TEST(test_orientation_actions_bearings_and_invalid_prompts);
}

SUITE_REGISTER(run_guided_step_suite);
