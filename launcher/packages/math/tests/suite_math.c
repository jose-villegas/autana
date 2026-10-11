/*
 * Portable suite: math/linear/'s float types. The quaternion's axes, slerp,
 * the matrix built from a TRS, the transform's matrix cache, the camera's
 * view matrix and look_at, each judged against a value worked out by hand.
 */

#include <math.h>
#include <stdbool.h>
#include <string.h>

#include "suites.h"
#include "transform_cache.h"
#include "unity.h"

#include "math/linear/transformf.h"
#include "math/linear/transformx.h"
#include "math/linear/vec_convert.h"

#define HALF_PI 1.57079632679F
#define SLACK   1e-5F

static void
assert_vec3(vec3f_t want, vec3f_t got) {
    TEST_ASSERT_FLOAT_WITHIN(SLACK, want.x, got.x);
    TEST_ASSERT_FLOAT_WITHIN(SLACK, want.y, got.y);
    TEST_ASSERT_FLOAT_WITHIN(SLACK, want.z, got.z);
}

static void
assert_mat4(const mat4f_t* want, const mat4f_t* got) {
    for (int r = 0; r < 4; r++) {
        for (int c = 0; c < 4; c++) {
            TEST_ASSERT_FLOAT_WITHIN(SLACK, want->m[r][c], got->m[r][c]);
        }
    }
}

static void
test_a_quarter_turn_about_y_takes_z_to_x(void) {
    const quatf_t q = quatf_from_euler((vec3f_t){0.0F, HALF_PI, 0.0F});
    assert_vec3((vec3f_t){1.0F, 0.0F, 0.0F}, quatf_rotate(q, (vec3f_t){0.0F, 0.0F, 1.0F}));
    assert_vec3((vec3f_t){0.0F, 1.0F, 0.0F}, quatf_rotate(q, (vec3f_t){0.0F, 1.0F, 0.0F}));
}

static void
test_a_quarter_turn_about_x_takes_y_to_z_and_about_z_takes_x_to_y(void) {
    assert_vec3((vec3f_t){0.0F, 0.0F, 1.0F},
                quatf_rotate(quatf_from_euler((vec3f_t){HALF_PI, 0.0F, 0.0F}), (vec3f_t){0.0F, 1.0F, 0.0F}));
    assert_vec3((vec3f_t){0.0F, 1.0F, 0.0F},
                quatf_rotate(quatf_from_euler((vec3f_t){0.0F, 0.0F, HALF_PI}), (vec3f_t){1.0F, 0.0F, 0.0F}));
}

static void
test_euler_angles_apply_z_then_x_then_y(void) {
    /* +x rolls to +y about z, which a quarter turn about x then takes to +z,
     * which a quarter turn about y takes to +x. Another order ends elsewhere. */
    const quatf_t q = quatf_from_euler((vec3f_t){HALF_PI, HALF_PI, HALF_PI});
    assert_vec3((vec3f_t){1.0F, 0.0F, 0.0F}, quatf_rotate(q, (vec3f_t){1.0F, 0.0F, 0.0F}));
    assert_vec3((vec3f_t){0.0F, 0.0F, 1.0F}, quatf_rotate(q, (vec3f_t){0.0F, 1.0F, 0.0F}));
}

static void
test_a_product_applies_the_right_operand_first(void) {
    const quatf_t about_y = quatf_from_axis_angle((vec3f_t){0.0F, 1.0F, 0.0F}, HALF_PI);
    const quatf_t about_x = quatf_from_axis_angle((vec3f_t){1.0F, 0.0F, 0.0F}, HALF_PI);
    assert_vec3((vec3f_t){1.0F, 0.0F, 0.0F}, quatf_rotate(quatf_mul(about_y, about_x), (vec3f_t){0.0F, 1.0F, 0.0F}));
}

static void
test_slerp_hits_both_ends_and_the_half_angle_between(void) {
    const quatf_t a = quatf_identity();
    const quatf_t b = quatf_from_axis_angle((vec3f_t){0.0F, 1.0F, 0.0F}, HALF_PI);
    const vec3f_t z = {0.0F, 0.0F, 1.0F};
    assert_vec3(quatf_rotate(a, z), quatf_rotate(quatf_slerp(a, b, 0.0F), z));
    assert_vec3(quatf_rotate(b, z), quatf_rotate(quatf_slerp(a, b, 1.0F), z));
    const quatf_t mid = quatf_slerp(a, b, 0.5F);
    assert_vec3((vec3f_t){sinf(HALF_PI / 2.0F), 0.0F, cosf(HALF_PI / 2.0F)}, quatf_rotate(mid, z));
    TEST_ASSERT_FLOAT_WITHIN(SLACK, 1.0F, (mid.x * mid.x) + (mid.y * mid.y) + (mid.z * mid.z) + (mid.w * mid.w));
}

static void
test_slerp_takes_the_short_way_when_the_signs_differ(void) {
    const quatf_t b = quatf_from_axis_angle((vec3f_t){0.0F, 1.0F, 0.0F}, HALF_PI);
    const quatf_t negated = {-b.x, -b.y, -b.z, -b.w};
    const quatf_t mid = quatf_slerp(quatf_identity(), negated, 0.5F);
    assert_vec3((vec3f_t){sinf(HALF_PI / 2.0F), 0.0F, cosf(HALF_PI / 2.0F)},
                quatf_rotate(mid, (vec3f_t){0.0F, 0.0F, 1.0F}));
}

static void
test_a_trs_matrix_is_scale_then_rotate_then_translate(void) {
    const quatf_t turn = quatf_from_axis_angle((vec3f_t){0.0F, 1.0F, 0.0F}, HALF_PI);
    const mat4f_t got = mat4f_from_trs((vec3f_t){1.0F, 2.0F, 3.0F}, turn, (vec3f_t){2.0F, 3.0F, 4.0F});
    /* A quarter turn about y sends x to -z and z to x. */
    const mat4f_t want = {{
        {0.0F, 0.0F, 4.0F, 1.0F},
        {0.0F, 3.0F, 0.0F, 2.0F},
        {-2.0F, 0.0F, 0.0F, 3.0F},
        {0.0F, 0.0F, 0.0F, 1.0F},
    }};
    assert_mat4(&want, &got);
}

/* A build is observed by overwriting the kept matrix with a sentinel: a call
 * that does not rebuild hands the sentinel back, one that does replaces it. */
#define SENTINEL 99.0F

static bool
rebuilds(transformf_t* t) {
    t->matrix.m[0][3] = SENTINEL;
    const bool rebuilt = transformf_matrix(t).m[0][3] != SENTINEL;
    if (rebuilt) {
        const mat4f_t want = mat4f_from_trs(t->position, t->rotation, t->scale);
        assert_mat4(&want, &t->matrix);
    }
    return rebuilt;
}

static void
test_a_transform_that_does_not_change_builds_its_matrix_once(void) {
    transformf_t t = {0};
    t.rotation = quatf_identity();
    t.scale = (vec3f_t){1.0F, 1.0F, 1.0F};
    transformf_set_position(&t, (vec3f_t){1.0F, 2.0F, 3.0F});
    TEST_ASSERT_FLOAT_WITHIN(SLACK, 1.0F, transformf_matrix(&t).m[0][3]);

    t.matrix.m[0][3] = SENTINEL;
    for (int i = 0; i < 5; i++) {
        TEST_ASSERT_EQUAL_FLOAT(SENTINEL, transformf_matrix(&t).m[0][3]);
    }
}

static void
test_a_zero_initialized_transform_builds_on_first_use(void) {
    transformf_t t = {0};
    t.scale = (vec3f_t){1.0F, 1.0F, 1.0F};
    t.rotation = quatf_identity();
    t.position = (vec3f_t){4.0F, 0.0F, 0.0F};
    TEST_ASSERT_FALSE(t.cached);
    TEST_ASSERT_FLOAT_WITHIN(SLACK, 4.0F, transformf_matrix(&t).m[0][3]);
    TEST_ASSERT_TRUE(t.cached);
}

static vec3f_t
to_f(float x, float y, float z) {
    return (vec3f_t){x, y, z};
}

static void
test_every_setter_translate_rotate_and_look_at_rebuilds_the_matrix(void) {
    transformf_t t = TRANSFORMF_IDENTITY;
    ASSERT_TRANSFORM_SETTERS(transformf, t, to_f, quatf_from_axis_angle((vec3f_t){0.0F, 1.0F, 0.0F}, HALF_PI),
                             rebuilds);
    transformf_look_at(&t, (vec3f_t){0.0F, 0.0F, 9.0F}, (vec3f_t){0.0F, 1.0F, 0.0F});
    TEST_ASSERT_TRUE(rebuilds(&t));
    transformf_rotate_around(&t, (vec3f_t){1.0F, 0.0F, 0.0F}, (vec3f_t){0.0F, 1.0F, 0.0F}, HALF_PI);
    TEST_ASSERT_TRUE(rebuilds(&t));
}

static void
test_compute_matrix_leaves_the_cache_alone(void) {
    transformf_t t = TRANSFORMF_IDENTITY;
    transformf_set_position(&t, (vec3f_t){7.0F, 0.0F, 0.0F});
    const mat4f_t fresh = transformf_compute_matrix(&t);
    TEST_ASSERT_FLOAT_WITHIN(SLACK, 7.0F, fresh.m[0][3]);
    TEST_ASSERT_FALSE(t.cached);
}

static void
assert_vec3f(vec3f_t want, vec3f_t got) {
    TEST_ASSERT_FLOAT_WITHIN(SLACK, want.x, got.x);
    TEST_ASSERT_FLOAT_WITHIN(SLACK, want.y, got.y);
    TEST_ASSERT_FLOAT_WITHIN(SLACK, want.z, got.z);
}

static void
test_rotate_around_swings_the_position_round_the_point_and_turns_the_rotation_with_it(void) {
    /* A quarter turn about +y through (5, 0, 0) takes +x to -z and +z to +x. */
    const vec3f_t point = {5.0F, 0.0F, 0.0F};
    transformf_t t = TRANSFORMF_IDENTITY;
    transformf_set_position(&t, (vec3f_t){6.0F, 0.0F, 0.0F});
    transformf_rotate_around(&t, point, (vec3f_t){0.0F, 2.0F, 0.0F}, HALF_PI); /* any axis length */
    assert_vec3f((vec3f_t){5.0F, 0.0F, -1.0F}, t.position);
    assert_vec3f((vec3f_t){1.0F, 0.0F, 0.0F}, quatf_rotate(t.rotation, (vec3f_t){0.0F, 0.0F, 1.0F}));
}

static void
test_rotate_around_its_own_position_only_turns(void) {
    transformf_t t = TRANSFORMF_IDENTITY;
    transformf_set_position(&t, (vec3f_t){1.0F, 2.0F, 3.0F});
    transformf_rotate_around(&t, t.position, (vec3f_t){1.0F, 0.0F, 0.0F}, HALF_PI);
    assert_vec3f((vec3f_t){1.0F, 2.0F, 3.0F}, t.position);
    assert_vec3f((vec3f_t){0.0F, -1.0F, 0.0F}, quatf_rotate(t.rotation, (vec3f_t){0.0F, 0.0F, 1.0F}));
}

static void
test_rotate_around_what_it_faces_keeps_facing_it_at_the_same_distance(void) {
    const vec3f_t point = {1.0F, -1.0F, 2.0F};
    transformf_t t = TRANSFORMF_IDENTITY;
    transformf_set_position(&t, (vec3f_t){4.0F, 3.0F, 2.0F});
    transformf_look_at(&t, point, (vec3f_t){0.0F, 1.0F, 0.0F});
    transformf_rotate_around(&t, point, (vec3f_t){0.3F, 1.0F, -0.5F}, 1.1F);
    const vec3f_t to_point = vec3f_sub(point, t.position);
    const float reach = sqrtf(vec3f_dot(to_point, to_point));
    TEST_ASSERT_FLOAT_WITHIN(SLACK, 5.0F, reach);
    assert_vec3f(vec3f_scale(to_point, 1.0F / reach), quatf_rotate(t.rotation, (vec3f_t){0.0F, 0.0F, 1.0F}));
}

static void
test_translate_adds_and_rotate_turns_about_the_local_axes(void) {
    transformf_t t = TRANSFORMF_IDENTITY;
    transformf_translate(&t, (vec3f_t){1.0F, 0.0F, 0.0F});
    transformf_translate(&t, (vec3f_t){0.0F, 2.0F, 0.0F});
    assert_vec3((vec3f_t){1.0F, 2.0F, 0.0F}, t.position);

    transformf_rotate(&t, quatf_from_axis_angle((vec3f_t){0.0F, 1.0F, 0.0F}, HALF_PI));
    transformf_rotate(&t, quatf_from_axis_angle((vec3f_t){1.0F, 0.0F, 0.0F}, HALF_PI));
    /* Local turns multiply on the right: Ry * Rx sends +z to -y. */
    assert_vec3((vec3f_t){0.0F, -1.0F, 0.0F}, quatf_rotate(t.rotation, (vec3f_t){0.0F, 0.0F, 1.0F}));
}

static void
test_the_view_matrix_inverts_a_rigid_model_matrix(void) {
    transformf_t t = TRANSFORMF_IDENTITY;
    transformf_set_position(&t, (vec3f_t){1.0F, -2.0F, 3.0F});
    transformf_set_rotation(&t, quatf_from_euler((vec3f_t){0.3F, -0.7F, 1.1F}));
    const mat4f_t model = transformf_matrix(&t);
    const mat4f_t view = transformf_view(&t);
    const mat4f_t identity = mat4f_identity();
    const mat4f_t round_trip = mat4f_mul(view, model);
    assert_mat4(&identity, &round_trip);
}

static void
test_the_view_matrix_puts_the_camera_at_the_origin_looking_down_z(void) {
    transformf_t camera = TRANSFORMF_IDENTITY;
    transformf_set_position(&camera, (vec3f_t){0.0F, 0.0F, -5.0F});
    const mat4f_t view = transformf_view(&camera);
    assert_vec3((vec3f_t){0.0F, 0.0F, 5.0F}, mat4f_apply(&view, (vec3f_t){0.0F, 0.0F, 0.0F}));
}

static void
test_look_at_points_local_z_at_the_target_with_up_kept_up(void) {
    const vec3f_t up = {0.0F, 1.0F, 0.0F};
    const vec3f_t targets[] = {{0.0F, 0.0F, 5.0F}, {5.0F, 0.0F, 0.0F}, {0.0F, 3.0F, 4.0F}, {-2.0F, -1.0F, -3.0F}};
    for (unsigned i = 0; i < sizeof(targets) / sizeof(targets[0]); i++) {
        transformf_t t = TRANSFORMF_IDENTITY;
        transformf_set_position(&t, (vec3f_t){1.0F, 1.0F, 1.0F});
        transformf_look_at(&t, targets[i], up);

        const vec3f_t forward = quatf_rotate(t.rotation, (vec3f_t){0.0F, 0.0F, 1.0F});
        assert_vec3(vec3f_normalize(vec3f_sub(targets[i], t.position)), forward);

        const vec3f_t right = quatf_rotate(t.rotation, (vec3f_t){1.0F, 0.0F, 0.0F});
        TEST_ASSERT_FLOAT_WITHIN(SLACK, 0.0F, right.y);
        TEST_ASSERT_TRUE(vec3f_dot(right, vec3f_cross(up, forward)) > 0.0F);
        TEST_ASSERT_TRUE(vec3f_dot(quatf_rotate(t.rotation, up), up) > 0.0F);
    }
}

static void
test_look_at_straight_back_is_a_half_turn(void) {
    transformf_t t = TRANSFORMF_IDENTITY;
    transformf_look_at(&t, (vec3f_t){0.0F, 0.0F, -4.0F}, (vec3f_t){0.0F, 1.0F, 0.0F});
    assert_vec3((vec3f_t){0.0F, 0.0F, -1.0F}, quatf_rotate(t.rotation, (vec3f_t){0.0F, 0.0F, 1.0F}));
    assert_vec3((vec3f_t){0.0F, 1.0F, 0.0F}, quatf_rotate(t.rotation, (vec3f_t){0.0F, 1.0F, 0.0F}));
}

static void
test_the_octahedral_map_ignores_length_and_round_trips(void) {
    const vec3f_t dirs[] = {{0.0F, 0.0F, 1.0F},  {0.0F, 0.0F, -1.0F},  {1.0F, 0.0F, 0.0F},  {0.0F, -1.0F, 0.0F},
                            {0.3F, -0.5F, 0.8F}, {-0.6F, 0.2F, -0.7F}, {0.1F, 0.9F, -0.4F}, {-0.7F, -0.7F, -0.1F}};
    for (unsigned i = 0; i < sizeof(dirs) / sizeof(dirs[0]); i++) {
        const vec2f_t p = vec3f_octahedral(dirs[i]);
        const vec2f_t scaled = vec3f_octahedral(vec3f_scale(dirs[i], 3.5F));
        TEST_ASSERT_TRUE(fabsf(p.x) <= 1.0F && fabsf(p.y) <= 1.0F);
        TEST_ASSERT_FLOAT_WITHIN(SLACK, p.x, scaled.x);
        TEST_ASSERT_FLOAT_WITHIN(SLACK, p.y, scaled.y);
        assert_vec3(vec3f_normalize(dirs[i]), vec3f_from_octahedral(p));
    }
}

static void
test_the_octahedral_map_puts_the_poles_at_the_centre_and_a_corner(void) {
    const vec2f_t up = vec3f_octahedral((vec3f_t){0.0F, 0.0F, 2.0F});
    const vec2f_t down = vec3f_octahedral((vec3f_t){0.0F, 0.0F, -2.0F});
    TEST_ASSERT_FLOAT_WITHIN(SLACK, 0.0F, up.x);
    TEST_ASSERT_FLOAT_WITHIN(SLACK, 0.0F, up.y);
    TEST_ASSERT_FLOAT_WITHIN(SLACK, 1.0F, fabsf(down.x));
    TEST_ASSERT_FLOAT_WITHIN(SLACK, 1.0F, fabsf(down.y));
}

static void
test_affine_product_and_inverse_match_full_matrices(void) {
    const mat4f_t a = mat4f_from_trs((vec3f_t){1.0F, -2.0F, 3.0F}, quatf_from_euler((vec3f_t){0.4F, -0.3F, 0.2F}),
                                     (vec3f_t){2.0F, 1.0F, 0.5F});
    const mat4f_t b = mat4f_from_trs((vec3f_t){-3.0F, 1.0F, 2.0F}, quatf_from_euler((vec3f_t){-0.2F, 0.5F, 0.3F}),
                                     (vec3f_t){0.5F, 3.0F, 2.0F});
    {
        const mat4f_t full = mat4f_mul(a, b), affine = mat4f_mul_affine(a, b);
        assert_vec3(mat4f_apply(&a, mat4f_apply(&b, (vec3f_t){1.0F, 1.0F, 1.0F})),
                    mat4f_apply(&full, (vec3f_t){1.0F, 1.0F, 1.0F}));
        assert_mat4(&full, &affine);
    }
    {
        const mat4f_t identity = mat4f_identity(), round_trip = mat4f_mul(mat4f_invert_affine(a), a);
        assert_mat4(&identity, &round_trip);
    }
}

static void
test_affine_inverse_preserves_small_and_large_scales(void) {
    const float scales[] = {1.0F / 64.0F, 64.0F, 256.0F};
    const float inverse_scales[] = {64.0F, 1.0F / 64.0F, 1.0F / 256.0F};
    const mat4f_t identity = mat4f_identity();
    for (unsigned i = 0; i < sizeof scales / sizeof scales[0]; i++) {
        for (int placed = 0; placed < 2; placed++) {
            const float scale = scales[i], inverse_scale = inverse_scales[i];
            mat4f_t m = identity, expected = identity;
            for (int axis = 0; axis < 3; axis++) {
                m.m[axis][axis] = scale;
                expected.m[axis][axis] = inverse_scale;
            }
            if (placed) {
                m.m[0][0] = m.m[1][1] = 0.0F;
                m.m[0][1] = -scale;
                m.m[1][0] = scale;
                m.m[0][3] = 1.0F;
                m.m[1][3] = -2.0F;
                m.m[2][3] = 3.0F;
                expected.m[0][0] = expected.m[1][1] = 0.0F;
                expected.m[0][1] = inverse_scale;
                expected.m[1][0] = -inverse_scale;
                expected.m[0][3] = 2.0F * inverse_scale;
                expected.m[1][3] = inverse_scale;
                expected.m[2][3] = -3.0F * inverse_scale;
            }
            const mat4f_t inverse = mat4f_invert_affine(m);
            const mat4f_t left = mat4f_mul_affine(inverse, m), right = mat4f_mul_affine(m, inverse);
            assert_mat4(&expected, &inverse);
            assert_mat4(&identity, &left);
            assert_mat4(&identity, &right);
        }
    }
}

#define CHECK_VEC2(A, U, got, X, Y, Z)                                                                                 \
    A((X) * (U), (got).x);                                                                                             \
    A((Y) * (U), (got).y)
#define CHECK_VEC3(A, U, got, X, Y, Z)                                                                                 \
    CHECK_VEC2(A, U, got, X, Y, Z);                                                                                    \
    A((Z) * (U), (got).z)
#define MAKE_VEC2(P, U, x, y, z) ((P##_t){(x) * (U), (y) * (U)})
#define MAKE_VEC3(P, U, x, y, z) ((P##_t){(x) * (U), (y) * (U), (z) * (U)})

#define VECTOR_RANGE_INPUTS(P, U, MAKE)                                                                                \
    const P##_t a = MAKE(P, U, -1, 8, 3), b = MAKE(P, U, 5, 2, 9);                                                     \
    const P##_t lo = MAKE(P, U, 0, 3, 4), hi = MAKE(P, U, 4, 6, 8), len = MAKE(P, U, 5, 3, 5);                         \
    (void)a;                                                                                                           \
    (void)b;                                                                                                           \
    (void)lo;                                                                                                          \
    (void)hi;                                                                                                          \
    (void)len;

#define DEFINE_VECTOR_RANGE_TEST(P, U, A, MAKE, CHECK, OP, X, Y, Z, ...)                                               \
    static void test_##P##_##OP(void) {                                                                                \
        VECTOR_RANGE_INPUTS(P, U, MAKE)                                                                                \
        const P##_t got = P##_##OP(__VA_ARGS__);                                                                       \
        CHECK(A, U, got, X, Y, Z);                                                                                     \
    }

#define DEFINE_VECTOR_RANGE_TESTS(P, U, A, MAKE, CHECK, HALF, END, ...)                                                \
    DEFINE_VECTOR_RANGE_TEST(P, U, A, MAKE, CHECK, min, -1, 2, 3, a, b)                                                \
    DEFINE_VECTOR_RANGE_TEST(P, U, A, MAKE, CHECK, max, 5, 8, 9, a, b)                                                 \
    DEFINE_VECTOR_RANGE_TEST(P, U, A, MAKE, CHECK, clamp, 0, 6, 4, a, lo, hi)                                          \
    DEFINE_VECTOR_RANGE_TEST(P, U, A, MAKE, CHECK, repeat, 4, 2, 3, a, len)                                            \
    DEFINE_VECTOR_RANGE_TEST(P, U, A, MAKE, CHECK, wrap, 3, 5, 7, a, lo, hi)                                           \
    static void test_##P##_lerp(void) {                                                                                \
        VECTOR_RANGE_INPUTS(P, U, MAKE)                                                                                \
        const P##_t got = P##_lerp(a, b, HALF __VA_ARGS__);                                                            \
        CHECK(A, U, got, 2, 5, 6);                                                                                     \
        TEST_ASSERT_TRUE(P##_equal(a, P##_lerp(a, b, 0 __VA_ARGS__)));                                                 \
        TEST_ASSERT_TRUE(P##_equal(b, P##_lerp(a, b, END __VA_ARGS__)));                                               \
    }

DEFINE_VECTOR_RANGE_TESTS(vec2f, 1, TEST_ASSERT_EQUAL_FLOAT, MAKE_VEC2, CHECK_VEC2, 0.5F, 1.0F, )
DEFINE_VECTOR_RANGE_TESTS(vec2i, 1, TEST_ASSERT_EQUAL_INT32, MAKE_VEC2, CHECK_VEC2, 1, 2, , 2)
DEFINE_VECTOR_RANGE_TESTS(vec2s, 1, TEST_ASSERT_EQUAL_INT32, MAKE_VEC2, CHECK_VEC2, 1, 2, , 2)
DEFINE_VECTOR_RANGE_TESTS(vec2x, MATHX_ONE, TEST_ASSERT_EQUAL_INT32, MAKE_VEC2, CHECK_VEC2, MATHX_ONE / 2, MATHX_ONE, )
DEFINE_VECTOR_RANGE_TESTS(vec3f, 1, TEST_ASSERT_EQUAL_FLOAT, MAKE_VEC3, CHECK_VEC3, 0.5F, 1.0F, )
DEFINE_VECTOR_RANGE_TESTS(vec3i, 1, TEST_ASSERT_EQUAL_INT32, MAKE_VEC3, CHECK_VEC3, 1, 2, , 2)
DEFINE_VECTOR_RANGE_TESTS(vec3s, 1, TEST_ASSERT_EQUAL_INT32, MAKE_VEC3, CHECK_VEC3, 1, 2, , 2)
DEFINE_VECTOR_RANGE_TESTS(vec3x, MATHX_ONE, TEST_ASSERT_EQUAL_INT32, MAKE_VEC3, CHECK_VEC3, MATHX_ONE / 2, MATHX_ONE, )

#define CHECK_QUAT_NEAR(A, S, want, got)                                                                               \
    A(S, (want).x, (got).x);                                                                                           \
    A(S, (want).y, (got).y);                                                                                           \
    A(S, (want).z, (got).z);                                                                                           \
    A(S, (want).w, (got).w)

static void
test_quatf_nlerp(void) {
    const quatf_t a = quatf_identity();
    const quatf_t b = {0.5F, 0.5F, 0.5F, 0.5F};
    const quatf_t neg = {-b.x, -b.y, -b.z, -b.w};
    const quatf_t got = quatf_nlerp(a, b, 0.5F);
    const quatf_t same = quatf_nlerp(a, neg, 0.5F);
    const quatf_t start = quatf_nlerp(a, b, 0);
    const quatf_t end = quatf_nlerp(a, b, 1.0F);
    TEST_ASSERT_FLOAT_WITHIN(SLACK, 1.0F, got.x * got.x + got.y * got.y + got.z * got.z + got.w * got.w);
    CHECK_QUAT_NEAR(TEST_ASSERT_FLOAT_WITHIN, SLACK, got, same);
    CHECK_QUAT_NEAR(TEST_ASSERT_FLOAT_WITHIN, SLACK, a, start);
    CHECK_QUAT_NEAR(TEST_ASSERT_FLOAT_WITHIN, SLACK, b, end);
}

static void
test_quatx_nlerp(void) {
    const quatx_t a = quatx_identity();
    const quatx_t b = {MATHX_ONE / 2, MATHX_ONE / 2, MATHX_ONE / 2, MATHX_ONE / 2};
    const quatx_t neg = {-b.x, -b.y, -b.z, -b.w};
    const quatx_t got = quatx_nlerp(a, b, MATHX_ONE / 2);
    const quatx_t same = quatx_nlerp(a, neg, MATHX_ONE / 2);
    const quatx_t start = quatx_nlerp(a, b, 0);
    const quatx_t end = quatx_nlerp(a, b, MATHX_ONE);
    TEST_ASSERT_INT32_WITHIN(4, MATHX_ONE,
                             mathx_add(mathx_add(mathx_mul(got.x, got.x), mathx_mul(got.y, got.y)),
                                       mathx_add(mathx_mul(got.z, got.z), mathx_mul(got.w, got.w))));
    CHECK_QUAT_NEAR(TEST_ASSERT_INT32_WITHIN, 4, got, same);
    CHECK_QUAT_NEAR(TEST_ASSERT_INT32_WITHIN, 4, a, start);
    CHECK_QUAT_NEAR(TEST_ASSERT_INT32_WITHIN, 4, b, end);
}

static void
test_quatf_slerp_preserves_float_results(void) {
    static const quatf_t expected[] = {
        {0x0.0000000000000p+0F, 0x0.0000000000000p+0F, 0x0.0000000000000p+0F, 0x1.0000000000000p+0F},
        {0x1.4799a60000000p-9F, 0x1.4799a60000000p-8F, -0x1.4799a60000000p-9F, 0x1.fffd8c0000000p-1F},
        {0x1.479b380000000p-8F, 0x1.479b380000000p-7F, -0x1.479b380000000p-8F, 0x1.fff62e0000000p-1F},
        {0x1.4794ee0000000p-7F, 0x1.4794ee0000000p-6F, -0x1.4794ee0000000p-7F, 0x1.ffd8b40000000p-1F},
        {0x0.0000000000000p+0F, 0x0.0000000000000p+0F, 0x0.0000000000000p+0F, 0x1.0000000000000p+0F},
        {0x1.3207f80000000p-3F, 0x1.3207f80000000p-3F, 0x1.3207f80000000p-3F, 0x1.ee8dd80000000p-1F},
        {0x1.279a740000000p-2F, 0x1.279a740000000p-2F, 0x1.279a740000000p-2F, 0x1.bb67ae0000000p-1F},
        {0x1.0000000000000p-1F, 0x1.0000000000000p-1F, 0x1.0000000000000p-1F, 0x1.0000000000000p-1F},
        {0x0.0000000000000p+0F, 0x0.0000000000000p+0F, 0x0.0000000000000p+0F, 0x1.0000000000000p+0F},
        {0x1.3207f80000000p-3F, 0x1.3207f80000000p-3F, 0x1.3207f80000000p-3F, 0x1.ee8dd80000000p-1F},
        {0x1.279a740000000p-2F, 0x1.279a740000000p-2F, 0x1.279a740000000p-2F, 0x1.bb67ae0000000p-1F},
        {0x1.0000000000000p-1F, 0x1.0000000000000p-1F, 0x1.0000000000000p-1F, 0x1.0000000000000p-1F},
        {0x0.0000000000000p+0F, 0x0.0000000000000p+0F, 0x0.0000000000000p+0F, 0x1.0000000000000p+0F},
        {0x0.0000000000000p+0F, 0x0.0000000000000p+0F, 0x0.0000000000000p+0F, 0x1.0000000000000p+0F},
        {0x0.0000000000000p+0F, 0x0.0000000000000p+0F, 0x0.0000000000000p+0F, 0x1.0000000000000p+0F},
        {0x0.0000000000000p+0F, 0x0.0000000000000p+0F, 0x0.0000000000000p+0F, 0x1.0000000000000p+0F},
    };
    const quatf_t bs[] = {
        {0.01F, 0.02F, -0.01F, 1.0F}, {0.5F, 0.5F, 0.5F, 0.5F}, {-0.5F, -0.5F, -0.5F, -0.5F}, {0, 0, 0, 1}};
    const float ts[] = {0.0F, 0.25F, 0.5F, 1.0F};
    for (size_t i = 0; i < sizeof bs / sizeof bs[0]; i++) {
        for (size_t j = 0; j < sizeof ts / sizeof ts[0]; j++) {
            const quatf_t got = quatf_slerp(quatf_identity(), quatf_normalize(bs[i]), ts[j]);
            const size_t index = i * (sizeof ts / sizeof ts[0]) + j;
            TEST_ASSERT_EQUAL_MEMORY(&expected[index], &got, sizeof got);
        }
    }
    static const quatf_t off_axis_expected[] = {
        {0x1.a43ea80000000p-4F, 0x1.a43da00000000p-3F, 0x1.3b2e380000000p-2F, 0x1.d8c5540000000p-1F},
    };
    const quatf_t a = quatf_normalize((quatf_t){0.1F, 0.2F, 0.3F, 0.9F});
    const quatf_t b = quatf_normalize((quatf_t){0.1F + 1.0F / 4096.0F, 0.2F, 0.3F, 0.9F});
    const float off_axis_weights[] = {1.0F / 256.0F};
    for (size_t i = 0; i < sizeof off_axis_weights / sizeof off_axis_weights[0]; i++) {
        const quatf_t got = quatf_slerp(a, b, off_axis_weights[i]);
        TEST_ASSERT_EQUAL_MEMORY(&off_axis_expected[i], &got, sizeof got);
    }
}

void
suite_math(void) {
    RUN_TEST(test_quatf_slerp_preserves_float_results);
    RUN_TEST(test_vec2f_min);
    RUN_TEST(test_vec2f_max);
    RUN_TEST(test_vec2f_clamp);
    RUN_TEST(test_vec2f_repeat);
    RUN_TEST(test_vec2f_wrap);
    RUN_TEST(test_vec2f_lerp);
    RUN_TEST(test_vec2i_min);
    RUN_TEST(test_vec2i_max);
    RUN_TEST(test_vec2i_clamp);
    RUN_TEST(test_vec2i_repeat);
    RUN_TEST(test_vec2i_wrap);
    RUN_TEST(test_vec2i_lerp);
    RUN_TEST(test_vec2s_min);
    RUN_TEST(test_vec2s_max);
    RUN_TEST(test_vec2s_clamp);
    RUN_TEST(test_vec2s_repeat);
    RUN_TEST(test_vec2s_wrap);
    RUN_TEST(test_vec2s_lerp);
    RUN_TEST(test_vec2x_min);
    RUN_TEST(test_vec2x_max);
    RUN_TEST(test_vec2x_clamp);
    RUN_TEST(test_vec2x_repeat);
    RUN_TEST(test_vec2x_wrap);
    RUN_TEST(test_vec2x_lerp);
    RUN_TEST(test_vec3f_min);
    RUN_TEST(test_vec3f_max);
    RUN_TEST(test_vec3f_clamp);
    RUN_TEST(test_vec3f_repeat);
    RUN_TEST(test_vec3f_wrap);
    RUN_TEST(test_vec3f_lerp);
    RUN_TEST(test_vec3i_min);
    RUN_TEST(test_vec3i_max);
    RUN_TEST(test_vec3i_clamp);
    RUN_TEST(test_vec3i_repeat);
    RUN_TEST(test_vec3i_wrap);
    RUN_TEST(test_vec3i_lerp);
    RUN_TEST(test_vec3s_min);
    RUN_TEST(test_vec3s_max);
    RUN_TEST(test_vec3s_clamp);
    RUN_TEST(test_vec3s_repeat);
    RUN_TEST(test_vec3s_wrap);
    RUN_TEST(test_vec3s_lerp);
    RUN_TEST(test_vec3x_min);
    RUN_TEST(test_vec3x_max);
    RUN_TEST(test_vec3x_clamp);
    RUN_TEST(test_vec3x_repeat);
    RUN_TEST(test_vec3x_wrap);
    RUN_TEST(test_vec3x_lerp);
    RUN_TEST(test_quatf_nlerp);
    RUN_TEST(test_quatx_nlerp);

    RUN_TEST(test_affine_inverse_preserves_small_and_large_scales);
    RUN_TEST(test_affine_product_and_inverse_match_full_matrices);
    RUN_TEST(test_a_quarter_turn_about_y_takes_z_to_x);
    RUN_TEST(test_a_quarter_turn_about_x_takes_y_to_z_and_about_z_takes_x_to_y);
    RUN_TEST(test_euler_angles_apply_z_then_x_then_y);
    RUN_TEST(test_a_product_applies_the_right_operand_first);
    RUN_TEST(test_slerp_hits_both_ends_and_the_half_angle_between);
    RUN_TEST(test_slerp_takes_the_short_way_when_the_signs_differ);
    RUN_TEST(test_a_trs_matrix_is_scale_then_rotate_then_translate);
    RUN_TEST(test_a_transform_that_does_not_change_builds_its_matrix_once);
    RUN_TEST(test_a_zero_initialized_transform_builds_on_first_use);
    RUN_TEST(test_every_setter_translate_rotate_and_look_at_rebuilds_the_matrix);
    RUN_TEST(test_compute_matrix_leaves_the_cache_alone);
    RUN_TEST(test_translate_adds_and_rotate_turns_about_the_local_axes);
    RUN_TEST(test_rotate_around_swings_the_position_round_the_point_and_turns_the_rotation_with_it);
    RUN_TEST(test_rotate_around_its_own_position_only_turns);
    RUN_TEST(test_rotate_around_what_it_faces_keeps_facing_it_at_the_same_distance);
    RUN_TEST(test_the_view_matrix_inverts_a_rigid_model_matrix);
    RUN_TEST(test_the_view_matrix_puts_the_camera_at_the_origin_looking_down_z);
    RUN_TEST(test_look_at_points_local_z_at_the_target_with_up_kept_up);
    RUN_TEST(test_look_at_straight_back_is_a_half_turn);
    RUN_TEST(test_the_octahedral_map_ignores_length_and_round_trips);
    RUN_TEST(test_the_octahedral_map_puts_the_poles_at_the_centre_and_a_corner);
}

SUITE_REGISTER(suite_math);
