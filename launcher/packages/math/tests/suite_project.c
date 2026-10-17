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
    PROJECT_X = -3,
    PROJECT_Y = 5,
    PROJECT_BEHIND_X = 7,
    PROJECT_BEHIND_Y = -5,
    PROJECT_BEHIND_Z = -2,
    PROJECT_FRONT_X = -4,
    PROJECT_FRONT_Y = 13,
    PROJECT_FRONT_Z = 14,
    PROJECT_CROSS_PARTS = 4,
    PROJECT_REMAINING_PARTS = 3,
};

static projectf_t
fixture(void) {
    return (projectf_t){PROJECT_FX, PROJECT_FY, PROJECT_CX, PROJECT_CY, PROJECT_NEAR};
}

static void
test_projectf_offset_preserves_camera_sign_and_independent_axes(void) {
    const projectf_t lens = fixture();
    const vec2f_t offset = projectf_offset(&lens, (vec3f_t){PROJECT_X, PROJECT_Y, PROJECT_DEPTH});
    TEST_ASSERT_EQUAL_FLOAT(PROJECT_FX * PROJECT_X / (float)PROJECT_DEPTH, offset.x);
    TEST_ASSERT_EQUAL_FLOAT(PROJECT_FY * PROJECT_Y / (float)PROJECT_DEPTH, offset.y);
}

static void
test_projectf_screen_center_and_independent_axes(void) {
    const projectf_t lens = fixture();
    const vec2f_t center = projectf_screen(&lens, (vec3f_t){0, 0, PROJECT_DEPTH});
    TEST_ASSERT_EQUAL_FLOAT(lens.cx, center.x);
    TEST_ASSERT_EQUAL_FLOAT(lens.cy, center.y);
    const vec2f_t offset = projectf_screen(&lens, (vec3f_t){PROJECT_X, PROJECT_Y, PROJECT_DEPTH});
    TEST_ASSERT_EQUAL_FLOAT(lens.cx + lens.fx * PROJECT_X / PROJECT_DEPTH, offset.x);
    TEST_ASSERT_EQUAL_FLOAT(lens.cy - lens.fy * PROJECT_Y / PROJECT_DEPTH, offset.y);
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

/* These depths put the crossing one quarter from behind, with distinct signed axes. */
static void
test_projectf_clip_near_crosses_in_both_orders(void) {
    const float scales[] = {1, MATHX_ONE};
    for (size_t i = 0; i < sizeof scales / sizeof scales[0]; i++) {
        const float scale = scales[i];
        for (int reverse = 0; reverse < 2; reverse++) {
            vec3f_t behind = {PROJECT_BEHIND_X * scale, PROJECT_BEHIND_Y * scale, PROJECT_BEHIND_Z * scale};
            vec3f_t front = {PROJECT_FRONT_X * scale, PROJECT_FRONT_Y * scale, PROJECT_FRONT_Z * scale};
            const vec3f_t before_front = front;
            TEST_ASSERT_TRUE(reverse ? projectf_clip_near(PROJECT_NEAR * scale, &front, &behind)
                                     : projectf_clip_near(PROJECT_NEAR * scale, &behind, &front));
            TEST_ASSERT_EQUAL_FLOAT(
                (PROJECT_REMAINING_PARTS * PROJECT_BEHIND_X + PROJECT_FRONT_X) * scale / PROJECT_CROSS_PARTS, behind.x);
            TEST_ASSERT_EQUAL_FLOAT(
                (PROJECT_REMAINING_PARTS * PROJECT_BEHIND_Y + PROJECT_FRONT_Y) * scale / PROJECT_CROSS_PARTS, behind.y);
            TEST_ASSERT_EQUAL_FLOAT(PROJECT_NEAR * scale, behind.z);
            TEST_ASSERT_EQUAL_MEMORY(&before_front, &front, sizeof front);
        }
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
            vec3x_t behind = {PROJECT_BEHIND_X * scales[i], PROJECT_BEHIND_Y * scales[i], PROJECT_BEHIND_Z * scales[i]};
            vec3x_t front = {PROJECT_FRONT_X * scales[i], PROJECT_FRONT_Y * scales[i], PROJECT_FRONT_Z * scales[i]};
            const vec3x_t before_front = front;
            TEST_ASSERT_TRUE(reverse ? projectx_clip_near(near_z, &front, &behind)
                                     : projectx_clip_near(near_z, &behind, &front));
            TEST_ASSERT_EQUAL_INT32((PROJECT_REMAINING_PARTS * PROJECT_BEHIND_X + PROJECT_FRONT_X) * scales[i]
                                        / PROJECT_CROSS_PARTS,
                                    behind.x);
            /* The negative y crossing floors: arithmetic right shift rounds toward minus infinity. */
            TEST_ASSERT_EQUAL_INT32(-((-(PROJECT_REMAINING_PARTS * PROJECT_BEHIND_Y + PROJECT_FRONT_Y) * scales[i]
                                       + PROJECT_CROSS_PARTS - 1)
                                      / PROJECT_CROSS_PARTS),
                                    behind.y);
            TEST_ASSERT_EQUAL_INT32(near_z, behind.z);
            TEST_ASSERT_EQUAL_MEMORY(&before_front, &front, sizeof front);
        }
    }
}

/* A zero-distance clip leaves public values unchanged; a callback makes endpoint selection observable. */
static void
record_cross_f(float near_z, vec3f_t* behind, const vec3f_t* front) {
    TEST_ASSERT_EQUAL_FLOAT(near_z, behind->z);
    *behind = *front;
}

static void
record_cross_x(int32_t near_z, vec3x_t* behind, const vec3x_t* front) {
    TEST_ASSERT_EQUAL_INT32(near_z, behind->z);
    *behind = *front;
}

MATH_DEFINE_CLIP_NEAR(record_f, vec3f_t, float, record_cross_f)
MATH_DEFINE_CLIP_NEAR(record_x, vec3x_t, int32_t, record_cross_x)

#define DEFINE_CHECK_AT_NEAR(NAME, V, T, CLIP, RECORD)                                                                 \
    static void NAME(int32_t scale, bool reverse) {                                                                    \
        const V at_near = {PROJECT_X * scale, PROJECT_Y * scale, PROJECT_NEAR * scale};                                \
        const V front = {PROJECT_FRONT_X * scale, PROJECT_FRONT_Y * scale, PROJECT_FRONT_Z * scale};                   \
        for (int record = 0; record < 2; record++) {                                                                   \
            V a = at_near, b = front;                                                                                  \
            bool (*clip)(T, V*, V*) = record ? RECORD : CLIP;                                                          \
            TEST_ASSERT_TRUE(reverse ? clip(at_near.z, &b, &a) : clip(at_near.z, &a, &b));                             \
            const V expected = record ? front : at_near;                                                               \
            TEST_ASSERT_EQUAL_MEMORY(&expected, &a, sizeof a);                                                         \
            TEST_ASSERT_EQUAL_MEMORY(&front, &b, sizeof b);                                                            \
        }                                                                                                              \
    }

DEFINE_CHECK_AT_NEAR(check_at_near_f, vec3f_t, float, projectf_clip_near, record_f_clip_near)
DEFINE_CHECK_AT_NEAR(check_at_near_x, vec3x_t, int32_t, projectx_clip_near, record_x_clip_near)

static void
test_at_near_endpoint_is_clipped_in_both_slots(void) {
    const int32_t scales[] = {1, MATHX_ONE};
    for (size_t i = 0; i < sizeof scales / sizeof scales[0]; i++) {
        for (int reverse = 0; reverse < 2; reverse++) {
            check_at_near_f(scales[i], reverse);
            check_at_near_x(scales[i], reverse);
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
    RUN_TEST(test_at_near_endpoint_is_clipped_in_both_slots);
}

SUITE_REGISTER(suite_project);
