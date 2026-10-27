#include "math/linear/transformf.h"
#include "testkit/suite.h"
#include "unity.h"

/* The decomposition of `matrix`, which rebuilds it. */
static transformf_t
assert_round_trip(mat4f_t matrix) {
    transformf_t out;
    TEST_ASSERT_TRUE(transformf_from_mat(matrix, &out));
    const mat4f_t reconstructed = transformf_compute_matrix(&out);
    for (int r = 0; r < 4; r++) {
        for (int c = 0; c < 4; c++) {
            TEST_ASSERT_FLOAT_WITHIN(TRANSFORMF_MATRIX_TOLERANCE, matrix.m[r][c], reconstructed.m[r][c]);
        }
    }
    return out;
}

static void
test_trs_matrix_decomposition_and_refusals(void) {
    const vec3f_t position = {3, 4, 5}, scale = {2, 3, 4};
    const quatf_t rotation = quatf_from_axis_angle((vec3f_t){0, 1, 0}, 0.7F);
    const mat4f_t matrix = mat4f_from_trs(position, rotation, scale);
    transformf_t out = assert_round_trip(matrix);
    TEST_ASSERT_EQUAL_FLOAT(3, out.position.x);
    TEST_ASSERT_FLOAT_WITHIN(TRANSFORMF_MATRIX_TOLERANCE, 4, out.scale.z);
    mat4f_t invalid = matrix;
    invalid.m[0][1] += 0.5F;
    TEST_ASSERT_FALSE(transformf_from_mat(invalid, &out));
    invalid = mat4f_from_trs(position, rotation, (vec3f_t){0, 3, 4});
    TEST_ASSERT_FALSE(transformf_from_mat(invalid, &out));
    invalid = matrix;
    invalid.m[3][0] = 1;
    TEST_ASSERT_FALSE(transformf_from_mat(invalid, &out));
}

/* A mirror on any axis comes back as a negative scale.x that rebuilds the
 * same matrix; mirroring twice is a turn, with every scale positive. */
static void
test_reflection_folds_into_a_negative_x_scale(void) {
    const vec3f_t position = {3, 4, 5};
    const quatf_t rotation = quatf_from_axis_angle((vec3f_t){0, 1, 0}, 0.7F);
    const vec3f_t mirrors[] = {{-2, 3, 4}, {2, -3, 4}, {2, 3, -4}};
    for (size_t i = 0; i < sizeof mirrors / sizeof mirrors[0]; i++) {
        const transformf_t out = assert_round_trip(mat4f_from_trs(position, rotation, mirrors[i]));
        TEST_ASSERT_LESS_THAN_FLOAT(0.0F, out.scale.x);
        TEST_ASSERT_GREATER_THAN_FLOAT(0.0F, out.scale.y);
        TEST_ASSERT_GREATER_THAN_FLOAT(0.0F, out.scale.z);
    }
    const transformf_t twice = assert_round_trip(mat4f_from_trs(position, rotation, (vec3f_t){-2, -3, 4}));
    TEST_ASSERT_GREATER_THAN_FLOAT(0.0F, twice.scale.x);
}

/* A parent turned a quarter about z and 1, 2, 3 out, scaled by `scale`;
 * its child 1 along x, turned about z and stretched along y. */
static void
compose_pair(vec3f_t scale, transformf_t* parent, transformf_t* child) {
    *parent = (transformf_t)TRANSFORMF_IDENTITY;
    transformf_set_position(parent, (vec3f_t){1.0F, 2.0F, 3.0F});
    transformf_set_rotation(parent, quatf_from_axis_angle((vec3f_t){0.0F, 0.0F, 1.0F}, MATH_PI / 2.0F));
    transformf_set_scale(parent, scale);
    *child = (transformf_t)TRANSFORMF_IDENTITY;
    transformf_set_position(child, (vec3f_t){1.0F, 0.0F, 0.0F});
    transformf_set_rotation(child, quatf_from_axis_angle((vec3f_t){0.0F, 0.0F, 1.0F}, MATH_PI / 3.0F));
    transformf_set_scale(child, (vec3f_t){1.0F, 3.0F, 1.0F});
}

/* The rows of the composed transform's matrix that match the product of
 * the parent's and the child's: columns 0 to `last`. */
static void
assert_compose_matches_product(vec3f_t scale, int last) {
    transformf_t parent, child;
    compose_pair(scale, &parent, &child);
    const transformf_t composed = transformf_compose(&parent, &child);
    TEST_ASSERT_FALSE(composed.cached);
    const mat4f_t want = mat4f_mul_affine(transformf_compute_matrix(&parent), transformf_compute_matrix(&child));
    const mat4f_t got = transformf_compute_matrix(&composed);
    for (int r = 0; r < 3; r++) {
        for (int c = last == 3 ? 0 : 3; c <= 3; c++) {
            TEST_ASSERT_FLOAT_WITHIN(1e-5F, want.m[r][c], got.m[r][c]);
        }
    }
}

static void
test_compose_under_an_evenly_scaled_parent_is_the_matrix_product(void) {
    assert_compose_matches_product((vec3f_t){2.0F, 2.0F, 2.0F}, 3);
}

/* An unevenly scaled parent over a turned child shears, which no TRS holds:
 * the position still matches, the matrix does not decompose. */
static void
test_compose_under_an_unevenly_scaled_parent_keeps_the_position(void) {
    assert_compose_matches_product((vec3f_t){2.0F, 1.0F, 1.0F}, -1);
    transformf_t parent, child, refused;
    compose_pair((vec3f_t){2.0F, 1.0F, 1.0F}, &parent, &child);
    TEST_ASSERT_FALSE(transformf_from_mat(
        mat4f_mul_affine(transformf_compute_matrix(&parent), transformf_compute_matrix(&child)), &refused));
}

/* relative undoes compose under an evenly scaled parent, and only an even
 * scale counts as even. */
static void
test_relative_undoes_compose_under_an_even_parent(void) {
    transformf_t parent, child;
    compose_pair((vec3f_t){2.0F, 2.0F, 2.0F}, &parent, &child);
    TEST_ASSERT_TRUE(transformf_scale_is_even(&parent));
    const transformf_t world = transformf_compose(&parent, &child);
    const transformf_t back = transformf_relative(&parent, &world);
    const mat4f_t want = transformf_compute_matrix(&child);
    const mat4f_t got = transformf_compute_matrix(&back);
    for (int r = 0; r < 3; r++) {
        for (int c = 0; c < 4; c++) {
            TEST_ASSERT_FLOAT_WITHIN(1e-5F, want.m[r][c], got.m[r][c]);
        }
    }
    transformf_set_scale(&parent, (vec3f_t){2.0F, 2.0F, 2.001F});
    TEST_ASSERT_FALSE(transformf_scale_is_even(&parent));
}

void
suite_transform_decompose(void) {
    RUN_TEST(test_relative_undoes_compose_under_an_even_parent);
    RUN_TEST(test_trs_matrix_decomposition_and_refusals);
    RUN_TEST(test_reflection_folds_into_a_negative_x_scale);
    RUN_TEST(test_compose_under_an_evenly_scaled_parent_is_the_matrix_product);
    RUN_TEST(test_compose_under_an_unevenly_scaled_parent_keeps_the_position);
}

SUITE_REGISTER(suite_transform_decompose);
