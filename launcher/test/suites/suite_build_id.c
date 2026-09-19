#include "suites.h"
#include "unity.h"

#include "util/build_id.h"

static void
test_format_clean_release(void) {
    char id[BUILD_ID_MAX];
    build_id_format(id, sizeof id, "1e720991ba86", false, "release");
    TEST_ASSERT_EQUAL_STRING("1e720991ba86-release", id);
}

static void
test_format_dirty_diag(void) {
    char id[BUILD_ID_MAX];
    build_id_format(id, sizeof id, "1e720991ba86", true, "diag");
    TEST_ASSERT_EQUAL_STRING("1e720991ba86-dirty-diag", id);
}

static void
test_line_has_fixed_prefix(void) {
    char line[BUILD_ID_LINE_MAX];
    build_id_line(line, sizeof line, "1e720991ba86-dev");
    TEST_ASSERT_EQUAL_STRING("BUILD_ID=1e720991ba86-dev", line);
}

static void
test_console_parser_recognizes_build_id(void) {
    TEST_ASSERT_EQUAL(BUILD_CONSOLE_BUILD_ID, build_console_command_parse("BUILDID"));
    TEST_ASSERT_EQUAL(BUILD_CONSOLE_NONE, build_console_command_parse("BUILD_ID"));
}

static void
test_console_parser_recognizes_touch(void) {
    TEST_ASSERT_EQUAL(BUILD_CONSOLE_TOUCH, build_console_command_parse("TOUCH down 10 20"));
    TEST_ASSERT_EQUAL(BUILD_CONSOLE_NONE, build_console_command_parse("TOUCH"));
    TEST_ASSERT_EQUAL(BUILD_CONSOLE_NONE, build_console_command_parse("TOUCHED down 10 20"));
}

static void
test_touch_line_carries_its_sample(void) {
    bool down = false;
    int x = 0, y = 0;

    TEST_ASSERT_TRUE(build_console_touch_parse("TOUCH down 184 224", &down, &x, &y));
    TEST_ASSERT_TRUE(down);
    TEST_ASSERT_EQUAL_INT(184, x);
    TEST_ASSERT_EQUAL_INT(224, y);

    TEST_ASSERT_TRUE(build_console_touch_parse("TOUCH up 7 9", &down, &x, &y));
    TEST_ASSERT_FALSE(down);
    TEST_ASSERT_EQUAL_INT(7, x);
    TEST_ASSERT_EQUAL_INT(9, y);
}

/* A half-read sample is worse than none: the caller would move a finger
 * somewhere nobody asked for. */
static void
test_a_malformed_touch_line_changes_nothing(void) {
    bool down = true;
    int x = 11, y = 22;

    /* Three shapes a line can be wrong in: a word that is neither state
     * but reads as one, a missing coordinate, and nothing at all. */
    TEST_ASSERT_FALSE(build_console_touch_parse("TOUCH left 1 2", &down, &x, &y));
    TEST_ASSERT_FALSE(build_console_touch_parse("TOUCH down 1", &down, &x, &y));
    TEST_ASSERT_FALSE(build_console_touch_parse("TOUCH  ", &down, &x, &y));
    TEST_ASSERT_TRUE(down);
    TEST_ASSERT_EQUAL_INT(11, x);
    TEST_ASSERT_EQUAL_INT(22, y);
}

/* The listener reads a line into a buffer this long, so a sample that
 * cannot fit it is a sample that never arrives whole. */
static void
test_a_touch_line_fits_the_console_buffer(void) {
    char line[BUILD_ID_LINE_MAX];
    const int written = snprintf(line, sizeof line, "TOUCH down %d %d", 448, 448);
    TEST_ASSERT_TRUE(written > 0 && written < (int)sizeof line);
}

void
suite_build_id(void) {
    RUN_TEST(test_format_clean_release);
    RUN_TEST(test_format_dirty_diag);
    RUN_TEST(test_line_has_fixed_prefix);
    RUN_TEST(test_console_parser_recognizes_build_id);
    RUN_TEST(test_console_parser_recognizes_touch);
    RUN_TEST(test_touch_line_carries_its_sample);
    RUN_TEST(test_a_malformed_touch_line_changes_nothing);
    RUN_TEST(test_a_touch_line_fits_the_console_buffer);
}

SUITE_REGISTER(suite_build_id)
