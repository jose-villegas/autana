/*
 * Device suite: settings and the system panel clock, against the real flash
 * store. The settings tests use a space of their own; the panel clock tests
 * use the shell's real key, so the user's value is saved first and put back.
 */

#include "suites.h"

#ifdef DEVICE_BUILD

#include <stdbool.h>
#include <stdint.h>

#include "display/display.h"
#include "display/display_shell.h"
#include "display/panel_clock.h"
#include "gfx/gfx.h"
#include "unity.h"
#include "util/settings.h"

#define TEST_SPACE     "test_settings"
#define SENTINEL       12345
#define UNSUPPORTED_HZ 1234567

static void
test_a_key_never_written_is_missing_and_leaves_the_output_alone(void) {
    int32_t out = 777;
    TEST_ASSERT_FALSE(settings_read_i32(TEST_SPACE, "never_written", &out));
    TEST_ASSERT_EQUAL_INT32(777, out);
}

static void
test_a_written_value_reads_back_and_a_rewrite_replaces_it(void) {
    int32_t out = 0;
    TEST_ASSERT_TRUE(settings_write_i32(TEST_SPACE, "round_trip", -42));
    TEST_ASSERT_TRUE(settings_read_i32(TEST_SPACE, "round_trip", &out));
    TEST_ASSERT_EQUAL_INT32(-42, out);
    TEST_ASSERT_TRUE(settings_write_i32(TEST_SPACE, "round_trip", 9));
    TEST_ASSERT_TRUE(settings_read_i32(TEST_SPACE, "round_trip", &out));
    TEST_ASSERT_EQUAL_INT32(9, out);
}

static int
other_rate(int hz) {
    return hz == PANEL_CLOCK_FAST_HZ ? PANEL_CLOCK_SLOW_HZ : PANEL_CLOCK_FAST_HZ;
}

static int32_t
stored_rate(void) {
    int32_t value = 0;
    TEST_ASSERT_TRUE(settings_read_i32(DISPLAY_PANEL_CLOCK_SETTINGS_SPACE, DISPLAY_PANEL_CLOCK_SETTINGS_KEY, &value));
    return value;
}

static void
test_a_supported_rate_is_applied_and_persisted(void) {
    const int hz = other_rate(display_system_panel_clock_hz());
    display_set_system_panel_clock_hz(hz);
    TEST_ASSERT_EQUAL_INT(hz, display_system_panel_clock_hz());
    TEST_ASSERT_EQUAL_INT32(hz, stored_rate());
}

static void
test_an_unchanged_rate_writes_nothing(void) {
    TEST_ASSERT_TRUE(
        settings_write_i32(DISPLAY_PANEL_CLOCK_SETTINGS_SPACE, DISPLAY_PANEL_CLOCK_SETTINGS_KEY, SENTINEL));
    display_set_system_panel_clock_hz(display_system_panel_clock_hz());
    TEST_ASSERT_EQUAL_INT32(SENTINEL, stored_rate());
}

static void
test_an_unsupported_rate_changes_and_writes_nothing(void) {
    const int before = display_system_panel_clock_hz();
    TEST_ASSERT_TRUE(
        settings_write_i32(DISPLAY_PANEL_CLOCK_SETTINGS_SPACE, DISPLAY_PANEL_CLOCK_SETTINGS_KEY, SENTINEL));
    display_set_system_panel_clock_hz(UNSUPPORTED_HZ);
    TEST_ASSERT_EQUAL_INT(before, display_system_panel_clock_hz());
    TEST_ASSERT_EQUAL_INT32(SENTINEL, stored_rate());
}

static void
test_restoring_returns_the_panel_to_the_system_rate(void) {
    const int system_hz = display_system_panel_clock_hz();
    TEST_ASSERT_TRUE(gfx_set_panel_clock_hz(other_rate(system_hz)));
    display_restore_system_state();
    TEST_ASSERT_EQUAL_INT(system_hz, gfx_panel_clock_hz());
}

#endif

void
run_settings_device_suite(void) {
#ifdef DEVICE_BUILD
    RUN_TEST(test_a_key_never_written_is_missing_and_leaves_the_output_alone);
    RUN_TEST(test_a_written_value_reads_back_and_a_rewrite_replaces_it);

    const int original_hz = display_system_panel_clock_hz();
    int32_t original_raw = 0;
    const bool had_raw =
        settings_read_i32(DISPLAY_PANEL_CLOCK_SETTINGS_SPACE, DISPLAY_PANEL_CLOCK_SETTINGS_KEY, &original_raw);
    RUN_TEST(test_a_supported_rate_is_applied_and_persisted);
    RUN_TEST(test_an_unchanged_rate_writes_nothing);
    RUN_TEST(test_an_unsupported_rate_changes_and_writes_nothing);
    RUN_TEST(test_restoring_returns_the_panel_to_the_system_rate);
    display_set_system_panel_clock_hz(original_hz);
    settings_write_i32(DISPLAY_PANEL_CLOCK_SETTINGS_SPACE, DISPLAY_PANEL_CLOCK_SETTINGS_KEY,
                       had_raw ? original_raw : original_hz);
#endif
}

SUITE_REGISTER(run_settings_device_suite);
