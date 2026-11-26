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

    const uint8_t expected_count = 3;
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
test_only_up_without_a_valid_point_reports_a_lift(void) {
    static const struct {
        uint8_t count;
        touch_event_t event;
        bool lifted;
    } cases[] = {
        {0, TOUCH_EVENT_UP, true},    {0, TOUCH_EVENT_DOWN, false}, {0, TOUCH_EVENT_CONTACT, false},
        {0, TOUCH_EVENT_NONE, false}, {1, TOUCH_EVENT_UP, false},   {5, TOUCH_EVENT_UP, false},
        {6, TOUCH_EVENT_UP, true},    {15, TOUCH_EVENT_UP, true},
    };

    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        const touch_report_t report = {.count = cases[i].count, .event = cases[i].event};
        TEST_ASSERT_EQUAL_INT(cases[i].lifted, touch_report_lifted(&report));
    }
}

static void
run_touch_report_suite(void) {
    RUN_TEST(test_report_decodes_all_events_and_masks_coordinate_high_bytes);
    RUN_TEST(test_only_counts_from_one_to_five_report_a_point);
    RUN_TEST(test_only_up_without_a_valid_point_reports_a_lift);
}

SUITE_REGISTER(run_touch_report_suite);
