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
test_console_parser_reads_a_touch(void) {
    bool down = false;
    int x = -1, y = -1;
    TEST_ASSERT_EQUAL(BUILD_CONSOLE_TOUCH, build_console_command_parse("TOUCH DOWN 120 300"));
    TEST_ASSERT_TRUE(build_console_parse_touch("TOUCH DOWN 120 300", &down, &x, &y));
    TEST_ASSERT_TRUE(down);
    TEST_ASSERT_EQUAL_INT(120, x);
    TEST_ASSERT_EQUAL_INT(300, y);

    TEST_ASSERT_TRUE(build_console_parse_touch("TOUCH UP", &down, &x, &y));
    TEST_ASSERT_FALSE(down);
}

static void
test_console_parser_rejects_a_malformed_touch(void) {
    bool down = true;
    int x = 7, y = 9;
    TEST_ASSERT_FALSE(build_console_parse_touch("TOUCH DOWN 120", &down, &x, &y));
    TEST_ASSERT_FALSE(build_console_parse_touch("TOUCH DOWN 120 300 4", &down, &x, &y));
    TEST_ASSERT_FALSE(build_console_parse_touch("TOUCH DOWN -1 300", &down, &x, &y));
    TEST_ASSERT_FALSE(build_console_parse_touch("TOUCH SIDEWAYS", &down, &x, &y));
    TEST_ASSERT_TRUE_MESSAGE(down && x == 7 && y == 9, "a rejected line must leave the outputs alone");
}

static void
test_console_parser_reads_an_imu_sample(void) {
    int ax = 0, ay = 0, az = 0;
    TEST_ASSERT_EQUAL(BUILD_CONSOLE_IMU, build_console_command_parse("IMU 4096 -12 0"));
    TEST_ASSERT_TRUE(build_console_parse_imu("IMU 4096 -12 0", &ax, &ay, &az));
    TEST_ASSERT_EQUAL_INT(4096, ax);
    TEST_ASSERT_EQUAL_INT(-12, ay);
    TEST_ASSERT_EQUAL_INT(0, az);

    TEST_ASSERT_FALSE(build_console_parse_imu("IMU 4096 0", &ax, &ay, &az));
    TEST_ASSERT_FALSE_MESSAGE(build_console_parse_imu("IMU 40000 0 0", &ax, &ay, &az),
                              "a count past int16 is not a sample the sensor could give");
}

void
suite_build_id(void) {
    RUN_TEST(test_format_clean_release);
    RUN_TEST(test_format_dirty_diag);
    RUN_TEST(test_line_has_fixed_prefix);
    RUN_TEST(test_console_parser_recognizes_build_id);
    RUN_TEST(test_console_parser_reads_a_touch);
    RUN_TEST(test_console_parser_rejects_a_malformed_touch);
    RUN_TEST(test_console_parser_reads_an_imu_sample);
}

SUITE_REGISTER(suite_build_id)
