#include "suites.h"
#include "unity.h"

#include "apps/sand/sand.h"
#include "apps/sand/sand_controls.h"
#include "input/motion/shake.h"

#define TEST_MID_LEVEL 0.5F

static shake_t
fixture(void) {
    return (shake_t){.smooth_g = SHAKE_FULL_G};
}

static void
test_jostle_mapping(void) {
    shake_t shake = fixture();
    const float sizes[] = {0, SHAKE_START_G / 2, SHAKE_START_G, SHAKE_FULL_G, SHAKE_FULL_G * 2};
    const int expected[] = {0, 0, 0, SAND_JOSTLE_MAX, SAND_JOSTLE_MAX};
    for (size_t i = 0; i < sizeof sizes / sizeof sizes[0]; i++) {
        shake.smooth_g = sizes[i];
        TEST_ASSERT_EQUAL_INT(expected[i], sand_controls_jostle(shake_level(&shake)));
    }
    shake.smooth_g = SHAKE_START_G + (TEST_MID_LEVEL * (SHAKE_FULL_G - SHAKE_START_G));
    TEST_ASSERT_EQUAL_INT((SAND_JOSTLE_MAX + 1) / 2, sand_controls_jostle(shake_level(&shake)));
    TEST_ASSERT_EQUAL_INT(0, sand_controls_jostle(-1));
    TEST_ASSERT_EQUAL_INT(SAND_JOSTLE_MAX, sand_controls_jostle(2));
}

void
run_sand_controls_suite(void) {
    RUN_TEST(test_jostle_mapping);
}

SUITE_REGISTER(run_sand_controls_suite);
