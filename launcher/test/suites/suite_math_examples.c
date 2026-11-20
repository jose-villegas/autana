#include <math.h>

#include "suites.h"
#include "unity.h"

#include "../math_examples/colour.c"
#include "../math_examples/transform.c"

#define SLACK 1e-5F

static void
test_documented_transform_centres_the_point_and_halves_the_rotation(void) {
    transform_example_t state = {0};
    transform_example(&state);
    TEST_ASSERT_FLOAT_WITHIN(SLACK, 0.0F, state.in_camera.x);
    TEST_ASSERT_FLOAT_WITHIN(SLACK, 0.0F, state.in_camera.y);
    const vec3f_t expected_world = {2.0F, 0.0F, 4.0F};
    TEST_ASSERT_FLOAT_WITHIN(SLACK, sqrtf(vec3f_dot(expected_world, expected_world)), state.in_camera.z);
    const vec3f_t turned = quatf_rotate(state.halfway, vec3f_unit_z());
    const float diagonal = sqrtf(0.5F);
    TEST_ASSERT_FLOAT_WITHIN(SLACK, diagonal, turned.x);
    TEST_ASSERT_FLOAT_WITHIN(SLACK, 0.0F, turned.y);
    TEST_ASSERT_FLOAT_WITHIN(SLACK, diagonal, turned.z);
}

#define GREY_RGB        0x777777U
#define LIGHTEN_STEP    0.1F
#define GREY_BYTE_SLACK 1
#define CHANNEL_MASK    0xFFU
#define WHITE_RGB       0xFFFFFFU

static void
test_documented_colour_lightens_a_grey_and_keeps_it_grey(void) {
    TEST_ASSERT_EQUAL_HEX32(GREY_RGB, colour_example_lighten(GREY_RGB, 0.0F));
    const uint32_t lighter = colour_example_lighten(GREY_RGB, LIGHTEN_STEP);
    const int r = (int)((lighter >> 16) & CHANNEL_MASK);
    const int g = (int)((lighter >> 8) & CHANNEL_MASK);
    const int b = (int)(lighter & CHANNEL_MASK);
    TEST_ASSERT_GREATER_THAN_INT((int)(GREY_RGB & CHANNEL_MASK), r);
    TEST_ASSERT_INT_WITHIN(GREY_BYTE_SLACK, r, g);
    TEST_ASSERT_INT_WITHIN(GREY_BYTE_SLACK, r, b);
    TEST_ASSERT_EQUAL_HEX32(WHITE_RGB, colour_example_lighten(GREY_RGB, 1.0F));
}

void
suite_math_examples(void) {
    RUN_TEST(test_documented_colour_lightens_a_grey_and_keeps_it_grey);
    RUN_TEST(test_documented_transform_centres_the_point_and_halves_the_rotation);
}

SUITE_REGISTER(suite_math_examples);
