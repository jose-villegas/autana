/*
 * suite_build_id: the BUILD_ID= line's fixed shape.
 */
#include "suites.h"
#include "unity.h"

#include "util/runtime/build_id.h"

static void
test_line_has_fixed_prefix(void) {
    char line[BUILD_ID_LINE_MAX];
    build_id_line(line, sizeof line, "0123456789ab-dev");
    TEST_ASSERT_EQUAL_STRING("BUILD_ID=0123456789ab-dev", line);
}

void
suite_build_id(void) {
    RUN_TEST(test_line_has_fixed_prefix);
}

SUITE_REGISTER(suite_build_id)
