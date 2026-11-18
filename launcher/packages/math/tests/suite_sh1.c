#include "math/sh/sh1.h"
#include "testkit/suite.h"
#include "unity.h"
#define SH_EPSILON 0.00001F

static void
test_sh_constant_and_lerp(void) {
    const vec3f_t radiance = {1, 2, 3};
    sh1_rgb_t sh = {0};
    const vec3f_t directions[] = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};
    const int count = sizeof directions / sizeof directions[0];
    for (int i = 0; i < count; i++) {
        sh1_rgb_add_direction(&sh, directions[i], vec3f_scale(radiance, 2.0F * MATH_TAU / count));
    }
    const sh1_irradiance_t e = sh1_rgb_irradiance(&sh);
    const vec3f_t normals[] = {{1, 0, 0}, {0, 1, 0}, {0, 0, -1}};
    for (int i = 0; i < 3; i++) {
        const vec3f_t got = sh1_irradiance_eval(&e, normals[i]);
        TEST_ASSERT_FLOAT_WITHIN(SH_EPSILON, MATH_PI * radiance.x, got.x);
        TEST_ASSERT_FLOAT_WITHIN(SH_EPSILON, MATH_PI * radiance.y, got.y);
        TEST_ASSERT_FLOAT_WITHIN(SH_EPSILON, MATH_PI * radiance.z, got.z);
    }
    const sh1_rgb_t zero = {0};
    const sh1_rgb_t a = sh1_rgb_lerp(sh, zero, 0);
    const sh1_rgb_t b = sh1_rgb_lerp(zero, sh, 1);
    TEST_ASSERT_EQUAL_MEMORY(&sh, &a, sizeof sh);
    TEST_ASSERT_EQUAL_MEMORY(&sh, &b, sizeof sh);
}

static void
test_sh_direction_closed_form(void) {
    sh1_rgb_t sh = {0};
    const vec3f_t d = vec3f_normalize((vec3f_t){1, 2, 3});
    sh1_rgb_add_direction(&sh, d, (vec3f_t){1, 2, 3});
    const sh1_irradiance_t e = sh1_rgb_irradiance(&sh);
    const vec3f_t plus = sh1_irradiance_eval(&e, d);
    const vec3f_t minus = sh1_irradiance_eval(&e, vec3f_scale(d, -1));
    const float constant = SH1_A0 * SH1_Y00 * SH1_Y00;
    const float linear = SH1_A1 * SH1_Y1 * SH1_Y1;
    TEST_ASSERT_FLOAT_WITHIN(SH_EPSILON, constant + linear, plus.x);
    TEST_ASSERT_FLOAT_WITHIN(SH_EPSILON, 2 * (constant - linear), minus.y);
    TEST_ASSERT_GREATER_THAN(minus.z, plus.z);
}

void
suite_sh1(void) {
    RUN_TEST(test_sh_constant_and_lerp);
    RUN_TEST(test_sh_direction_closed_form);
}

SUITE_REGISTER(suite_sh1);
