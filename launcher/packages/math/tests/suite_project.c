#include "suites.h"
#include "unity.h"

#include "math/linear/projectf.h"
#include "math/linear/projectx.h"

enum {
    PROJECT_FX = 12,
    PROJECT_FY = 20,
    PROJECT_CX = 31,
    PROJECT_CY = 43,
    PROJECT_NEAR = 2,
    PROJECT_DEPTH = 4,
};

static projectf_t
fixture(void) {
    return (projectf_t){PROJECT_FX, PROJECT_FY, PROJECT_CX, PROJECT_CY, PROJECT_NEAR};
}

static void
test_projectf_offset_preserves_camera_sign_and_independent_axes(void) {
    const projectf_t lens = fixture();
    const vec2f_t offset = projectf_offset(&lens, (vec3f_t){-PROJECT_NEAR, PROJECT_NEAR, PROJECT_DEPTH});
    TEST_ASSERT_EQUAL_FLOAT(-PROJECT_FX * PROJECT_NEAR / (float)PROJECT_DEPTH, offset.x);
    TEST_ASSERT_EQUAL_FLOAT(PROJECT_FY * PROJECT_NEAR / (float)PROJECT_DEPTH, offset.y);
}

static void
test_projectf_screen_center_and_independent_axes(void) {
    const projectf_t lens = fixture();
    const vec2f_t center = projectf_screen(&lens, (vec3f_t){0, 0, PROJECT_DEPTH});
    TEST_ASSERT_EQUAL_FLOAT(lens.cx, center.x);
    TEST_ASSERT_EQUAL_FLOAT(lens.cy, center.y);
    const vec2f_t offset = projectf_screen(&lens, (vec3f_t){PROJECT_DEPTH, PROJECT_DEPTH, PROJECT_DEPTH});
    TEST_ASSERT_EQUAL_FLOAT(lens.cx + lens.fx, offset.x);
    TEST_ASSERT_EQUAL_FLOAT(lens.cy - lens.fy, offset.y);
}

static void
test_projectf_clip_near_rejects_and_preserves_endpoints(void) {
    const projectf_t lens = fixture();
    vec3f_t a = {0, 0, 0}, b = {0, 0, lens.near_z};
    const vec3f_t before_a = a, before_b = b;
    TEST_ASSERT_FALSE(projectf_clip_near(lens.near_z, &a, &b));
    TEST_ASSERT_EQUAL_MEMORY(&before_a, &a, sizeof a);
    TEST_ASSERT_EQUAL_MEMORY(&before_b, &b, sizeof b);
    a.z = b.z = PROJECT_DEPTH;
    const vec3f_t front_a = a, front_b = b;
    TEST_ASSERT_TRUE(projectf_clip_near(lens.near_z, &a, &b));
    TEST_ASSERT_EQUAL_MEMORY(&front_a, &a, sizeof a);
    TEST_ASSERT_EQUAL_MEMORY(&front_b, &b, sizeof b);
}

static void
test_projectf_clip_near_crosses_in_both_orders(void) {
    const projectf_t lens = fixture();
    for (int reverse = 0; reverse < 2; reverse++) {
        vec3f_t behind = {0, 0, 0}, front = {PROJECT_DEPTH, -PROJECT_DEPTH, PROJECT_DEPTH};
        const vec3f_t before_front = front;
        TEST_ASSERT_TRUE(reverse ? projectf_clip_near(lens.near_z, &front, &behind)
                                 : projectf_clip_near(lens.near_z, &behind, &front));
        TEST_ASSERT_EQUAL_FLOAT(lens.near_z, behind.x);
        TEST_ASSERT_EQUAL_FLOAT(-lens.near_z, behind.y);
        TEST_ASSERT_EQUAL_FLOAT(lens.near_z, behind.z);
        TEST_ASSERT_EQUAL_MEMORY(&before_front, &front, sizeof front);
    }
}

static void
test_projectx_clip_near_at_two_scales(void) {
    const int32_t scales[] = {1, MATHX_ONE};
    for (unsigned i = 0; i < sizeof scales / sizeof scales[0]; i++) {
        const int32_t near_z = PROJECT_NEAR * scales[i];
        vec3x_t a = {0, 0, 0}, b = {0, 0, near_z};
        const vec3x_t before_a = a, before_b = b;
        TEST_ASSERT_FALSE(projectx_clip_near(near_z, &a, &b));
        TEST_ASSERT_EQUAL_MEMORY(&before_a, &a, sizeof a);
        TEST_ASSERT_EQUAL_MEMORY(&before_b, &b, sizeof b);
        a.z = b.z = PROJECT_DEPTH * scales[i];
        const vec3x_t front_a = a, front_b = b;
        TEST_ASSERT_TRUE(projectx_clip_near(near_z, &a, &b));
        TEST_ASSERT_EQUAL_MEMORY(&front_a, &a, sizeof a);
        TEST_ASSERT_EQUAL_MEMORY(&front_b, &b, sizeof b);
        for (int reverse = 0; reverse < 2; reverse++) {
            vec3x_t behind = {0, 0, 0};
            vec3x_t front = {PROJECT_DEPTH * scales[i], -PROJECT_DEPTH * scales[i], PROJECT_DEPTH * scales[i]};
            const vec3x_t before_front = front;
            TEST_ASSERT_TRUE(reverse ? projectx_clip_near(near_z, &front, &behind)
                                     : projectx_clip_near(near_z, &behind, &front));
            TEST_ASSERT_EQUAL_INT32(near_z, behind.x);
            TEST_ASSERT_EQUAL_INT32(-near_z, behind.y);
            TEST_ASSERT_EQUAL_INT32(near_z, behind.z);
            TEST_ASSERT_EQUAL_MEMORY(&before_front, &front, sizeof front);
        }
    }
}

void
suite_project(void) {
    RUN_TEST(test_projectf_offset_preserves_camera_sign_and_independent_axes);
    RUN_TEST(test_projectf_screen_center_and_independent_axes);
    RUN_TEST(test_projectf_clip_near_rejects_and_preserves_endpoints);
    RUN_TEST(test_projectf_clip_near_crosses_in_both_orders);
    RUN_TEST(test_projectx_clip_near_at_two_scales);
}

SUITE_REGISTER(suite_project);
