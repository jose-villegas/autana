#include "input/touch/touch_fsm.h"
#include "input/touch/touch_report.h"
#include "suites.h"
#include "unity.h"

static void
test_report_decodes_all_events_and_masks_coordinate_high_bytes(void) {
    static const struct {
        uint8_t raw[TOUCH_REPORT_BYTES];
        touch_event_t event;
    } cases[] = {
        {{0xf3, 0x3a, 0xbc, 0xfd, 0xef}, TOUCH_EVENT_DOWN},
        {{0xf3, 0x7a, 0xbc, 0xfd, 0xef}, TOUCH_EVENT_UP},
        {{0xf3, 0xba, 0xbc, 0xfd, 0xef}, TOUCH_EVENT_CONTACT},
        {{0xf3, 0xfa, 0xbc, 0xfd, 0xef}, TOUCH_EVENT_NONE},
    };

    const uint8_t expected_count = 0xf3;
    const vec2i_t expected_point = {0xabc, 0xdef};

    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        touch_report_t report;
        touch_report_decode(cases[i].raw, &report);

        TEST_ASSERT_EQUAL_INT(cases[i].event, report.event);
        TEST_ASSERT_EQUAL_INT(expected_count, report.count);
        TEST_ASSERT_EQUAL_INT(expected_point.x, report.point.x);
        TEST_ASSERT_EQUAL_INT(expected_point.y, report.point.y);
    }
}

static void
test_only_counts_from_one_to_five_report_a_point(void) {
    static const bool has_point_by_count[] = {
        false, true, true, true, true, true, false, false, false, false, false, false, false, false, false, false,
    };
    for (uint8_t count = 0; count < sizeof has_point_by_count / sizeof has_point_by_count[0]; count++) {
        const touch_report_t report = {.count = count, .event = TOUCH_EVENT_CONTACT};
        TEST_ASSERT_EQUAL_INT(has_point_by_count[count], touch_report_has_point(&report));
    }
}

static void
test_only_up_with_zero_count_reports_a_lift(void) {
    static const struct {
        uint8_t count;
        touch_event_t event;
        bool lifted;
    } cases[] = {
        {0, TOUCH_EVENT_UP, true},    {0, TOUCH_EVENT_DOWN, false}, {0, TOUCH_EVENT_CONTACT, false},
        {0, TOUCH_EVENT_NONE, false}, {1, TOUCH_EVENT_UP, false},   {5, TOUCH_EVENT_UP, false},
        {6, TOUCH_EVENT_UP, false},   {15, TOUCH_EVENT_UP, false},
    };

    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        const touch_report_t report = {.count = cases[i].count, .event = cases[i].event};
        TEST_ASSERT_EQUAL_INT(cases[i].lifted, touch_report_lifted(&report));
    }
}

static void
test_up_with_only_high_count_bits_is_neither_a_point_nor_a_lift(void) {
    static const uint8_t cases[][TOUCH_REPORT_BYTES] = {
        {0x10, 0x40, 0x00, 0x00, 0x00},
        {0xf0, 0x40, 0x00, 0x00, 0x00},
    };

    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        touch_report_t report;
        touch_report_decode(cases[i], &report);

        TEST_ASSERT_FALSE(touch_report_has_point(&report));
        TEST_ASSERT_FALSE(touch_report_lifted(&report));
    }
}

static const uint8_t first_contact[TOUCH_REPORT_BYTES] = {0x01, 0x00, 0x12, 0x00, 0x34};
static const uint8_t held_contact[TOUCH_REPORT_BYTES] = {0x01, 0x80, 0x23, 0x00, 0x45};
static const uint8_t lift[TOUCH_REPORT_BYTES] = {0x00, 0x40, 0x00, 0x00, 0x00};

static input_t
take_raw_sample(touch_fsm_t* fsm, const uint8_t raw[TOUCH_REPORT_BYTES], int64_t now_us) {
    touch_report_t report;
    input_t input;
    touch_report_decode(raw, &report);
    touch_fsm_update(fsm, touch_report_has_point(&report), touch_report_lifted(&report), report.point, now_us);
    touch_fsm_take(fsm, &input);
    return input;
}

static void
test_raw_reports_release_only_on_lift_or_after_quiet_window(void) {
    static const struct {
        uint8_t raw[TOUCH_REPORT_BYTES];
        bool releases_early;
    } cases[] = {
        {{0x00, 0x40, 0x00, 0x00, 0x00}, true},
        {{0x00, 0x80, 0x00, 0x00, 0x00}, false},
        {{0x07, 0x40, 0x00, 0x00, 0x00}, false},
        {{0x10, 0x40, 0x00, 0x00, 0x00}, false},
    };

    const uint8_t point_with_up[TOUCH_REPORT_BYTES] = {0x01, 0x40, 0x23, 0x00, 0x45};
    const int64_t step_us = TOUCH_RELEASE_QUIET_US / 4;
    const int64_t last_contact_us = 2 * step_us;

    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        touch_fsm_t fsm;
        touch_fsm_init(&fsm);
        take_raw_sample(&fsm, first_contact, 0);
        take_raw_sample(&fsm, held_contact, step_us);

        input_t in = take_raw_sample(&fsm, point_with_up, last_contact_us);
        TEST_ASSERT_TRUE(in.down);
        TEST_ASSERT_FALSE(in.released);

        in = take_raw_sample(&fsm, cases[i].raw, last_contact_us + step_us);
        TEST_ASSERT_EQUAL_INT(cases[i].releases_early, in.released);
        TEST_ASSERT_EQUAL_INT(!cases[i].releases_early, in.down);

        in = take_raw_sample(&fsm, cases[i].raw, last_contact_us + TOUCH_RELEASE_QUIET_US);
        TEST_ASSERT_FALSE(in.released);
        TEST_ASSERT_EQUAL_INT(!cases[i].releases_early, in.down);

        in = take_raw_sample(&fsm, cases[i].raw, last_contact_us + TOUCH_RELEASE_QUIET_US + 1);
        TEST_ASSERT_EQUAL_INT(!cases[i].releases_early, in.released);
        TEST_ASSERT_FALSE(in.down);
    }
}

static void
test_consecutive_lift_reports_produce_one_release_edge(void) {
    const int lift_samples = 4;
    const int64_t step_us = TOUCH_RELEASE_QUIET_US / (lift_samples + 1);
    touch_fsm_t fsm;
    touch_fsm_init(&fsm);
    take_raw_sample(&fsm, first_contact, 0);

    int releases = 0;
    for (int i = 1; i <= lift_samples; i++) {
        const input_t in = take_raw_sample(&fsm, lift, i * step_us);
        releases += in.released;
    }
    TEST_ASSERT_EQUAL_INT(1, releases);
}

static void
test_lift_and_timed_release_keep_the_same_press_and_point(void) {
    const uint8_t dropout[TOUCH_REPORT_BYTES] = {0x00, 0x80, 0x00, 0x00, 0x00};
    const int64_t step_us = TOUCH_RELEASE_QUIET_US / 4;
    touch_fsm_t fsm;
    touch_fsm_init(&fsm);
    take_raw_sample(&fsm, first_contact, 0);
    take_raw_sample(&fsm, held_contact, step_us);

    touch_fsm_t timed_fsm = fsm;
    const input_t lifted = take_raw_sample(&fsm, lift, 2 * step_us);
    const input_t timed = take_raw_sample(&timed_fsm, dropout, step_us + TOUCH_RELEASE_QUIET_US + 1);

    TEST_ASSERT_TRUE(lifted.released);
    TEST_ASSERT_TRUE(timed.released);
    TEST_ASSERT_EQUAL_INT(timed.press.x, lifted.press.x);
    TEST_ASSERT_EQUAL_INT(timed.press.y, lifted.press.y);
    TEST_ASSERT_EQUAL_INT(timed.point.x, lifted.point.x);
    TEST_ASSERT_EQUAL_INT(timed.point.y, lifted.point.y);
}

static void
run_touch_report_suite(void) {
    RUN_TEST(test_report_decodes_all_events_and_masks_coordinate_high_bytes);
    RUN_TEST(test_only_counts_from_one_to_five_report_a_point);
    RUN_TEST(test_only_up_with_zero_count_reports_a_lift);
    RUN_TEST(test_up_with_only_high_count_bits_is_neither_a_point_nor_a_lift);
    RUN_TEST(test_raw_reports_release_only_on_lift_or_after_quiet_window);
    RUN_TEST(test_consecutive_lift_reports_produce_one_release_edge);
    RUN_TEST(test_lift_and_timed_release_keep_the_same_press_and_point);
}

SUITE_REGISTER(run_touch_report_suite);
