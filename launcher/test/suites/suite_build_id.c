#include "suites.h"
#include "unity.h"

#include "util/build_id.h"

static void
test_line_has_fixed_prefix(void) {
    char line[BUILD_ID_LINE_MAX];
    build_id_line(line, sizeof line, "20260929T031205-7f3a9b2c-dev");
    TEST_ASSERT_EQUAL_STRING("BUILD_ID=20260929T031205-7f3a9b2c-dev", line);
}

void
suite_build_id(void) {
    RUN_TEST(test_line_has_fixed_prefix);
}

SUITE_REGISTER(suite_build_id)
