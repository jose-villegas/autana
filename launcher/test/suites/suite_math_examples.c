#include <math.h>

#include "suites.h"
#include "unity.h"

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
    const vec3f_t turned = quatf_rotate(state.halfway, (vec3f_t){0.0F, 0.0F, 1.0F});
    const float diagonal = sqrtf(0.5F);
    TEST_ASSERT_FLOAT_WITHIN(SLACK, diagonal, turned.x);
    TEST_ASSERT_FLOAT_WITHIN(SLACK, 0.0F, turned.y);
    TEST_ASSERT_FLOAT_WITHIN(SLACK, diagonal, turned.z);
}

void
suite_math_examples(void) {
    RUN_TEST(test_documented_transform_centres_the_point_and_halves_the_rotation);
}

SUITE_REGISTER(suite_math_examples);
