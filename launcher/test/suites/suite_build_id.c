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

void
suite_build_id(void) {
    RUN_TEST(test_format_clean_release);
    RUN_TEST(test_format_dirty_diag);
    RUN_TEST(test_line_has_fixed_prefix);
    RUN_TEST(test_console_parser_recognizes_build_id);
}

SUITE_REGISTER(suite_build_id)
