#include <float.h>

#include "math/linear/transformf.h"
#include "testkit/suite.h"
#include "unity.h"

static void
test_trs_matrix_decomposition_and_refusals(void) {
    const vec3f_t position = {3, 4, 5}, scale = {2, 3, 4};
    const quatf_t rotation = quatf_from_axis_angle(vec3f_unit_y(), 0.7F);
    const mat4f_t matrix = mat4f_from_trs(position, rotation, scale);
    transformf_t out;
    TEST_ASSERT_TRUE(transformf_from_mat(matrix, &out));
    TEST_ASSERT_EQUAL_FLOAT(3, out.position.x);
    TEST_ASSERT_FLOAT_WITHIN(TRANSFORMF_MATRIX_TOLERANCE, 4, out.scale.z);
    const mat4f_t reconstructed = transformf_compute_matrix(&out);
    for (int r = 0; r < 4; r++) {
        for (int c = 0; c < 4; c++) {
            TEST_ASSERT_FLOAT_WITHIN(TRANSFORMF_MATRIX_TOLERANCE, matrix.m[r][c], reconstructed.m[r][c]);
        }
    }
    mat4f_t invalid = matrix;
    invalid.m[0][1] += 0.5F;
    TEST_ASSERT_FALSE(transformf_from_mat(invalid, &out));
    invalid = mat4f_from_trs(position, rotation, (vec3f_t){-2, 3, 4});
    TEST_ASSERT_FALSE(transformf_from_mat(invalid, &out));
    invalid = mat4f_from_trs(position, rotation, (vec3f_t){0, 3, 4});
    TEST_ASSERT_FALSE(transformf_from_mat(invalid, &out));
    invalid = matrix;
    invalid.m[3][0] = 1;
    TEST_ASSERT_FALSE(transformf_from_mat(invalid, &out));
}

/* A parent turned a quarter about z and 1, 2, 3 out, scaled by `scale`;
 * its child 1 along x, turned about z and stretched along y. */
static void
compose_pair(vec3f_t scale, transformf_t* parent, transformf_t* child) {
    *parent = transformf_identity();
    transformf_set_position(parent, (vec3f_t){1.0F, 2.0F, 3.0F});
    transformf_set_rotation(parent, quatf_from_axis_angle(vec3f_unit_z(), MATH_PI / 2.0F));
    transformf_set_scale(parent, scale);
    *child = transformf_identity();
    transformf_set_position(child, vec3f_unit_x());
    transformf_set_rotation(child, quatf_from_axis_angle(vec3f_unit_z(), MATH_PI / 3.0F));
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

static void
test_inverse_point_under_an_uneven_parent(void) {
    transformf_t parent, child;
    compose_pair((vec3f_t){2.0F, -3.0F, 4.0F}, &parent, &child);
    const vec3f_t point = {2.0F, 3.0F, -1.0F};
    const mat4f_t matrix = transformf_compute_matrix(&parent);
    const vec3f_t world = mat4f_apply(&matrix, point);
    const vec3f_t actual = transformf_inverse_point(&parent, world);
    TEST_ASSERT_FLOAT_WITHIN(TRANSFORMF_MATRIX_TOLERANCE, point.x, actual.x);
    TEST_ASSERT_FLOAT_WITHIN(TRANSFORMF_MATRIX_TOLERANCE, point.y, actual.y);
    TEST_ASSERT_FLOAT_WITHIN(TRANSFORMF_MATRIX_TOLERANCE, point.z, actual.z);
}

/* A fused multiply-add can round a blend component one ULP differently. */
static void
assert_blend_vec3(vec3f_t expected, vec3f_t actual) {
    TEST_ASSERT_FLOAT_WITHIN(FLT_EPSILON * fabsf(expected.x), expected.x, actual.x);
    TEST_ASSERT_FLOAT_WITHIN(FLT_EPSILON * fabsf(expected.y), expected.y, actual.y);
    TEST_ASSERT_FLOAT_WITHIN(FLT_EPSILON * fabsf(expected.z), expected.z, actual.z);
}

static void
test_blend_clamps_and_interpolates_trs(void) {
    transformf_t a = {.position = {-3.7F, 0.3F, -4.9F},
                      .rotation = quatf_normalize((quatf_t){1, -2, 3, 4}),
                      .scale = {1.3F, -0.9F, 7.2F}};
    const transformf_t b = {.position = {-0.2F, -16.7F, 0.3F},
                            .rotation = quatf_normalize((quatf_t){-4, 3, -2, -1}),
                            .scale = {-9.1F, 0.3F, 0.2F}};
    (void)transformf_matrix(&a);

    static const struct {
        float weight;
        vec3f_t position, scale;
    } cases[] = {
        {0.3F, {-0x1.533332p+1F, -0x1.333334p+2F, -0x1.ab8520p+1F}, {-0x1.d1eb8ap+0F, -0x1.147ae0p-1F, 0x1.466666p+2F}},
        {1.0F / 3.0F,
         {-0x1.444442p+1F, -0x1.57777ap+2F, -0x1.955556p+1F},
         {-0x1.155558p+1F, -0x1.fffffcp-2F, 0x1.377776p+2F}},
        {0.999F, {-0x1.a0c496p-3F, -0x1.0aed94p+4F, 0x1.2de012p-2F}, {-0x1.22de02p+3F, 0x1.31f8a2p-2F, 0x1.a7ef92p-3F}},
    };

    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        const transformf_t got = transformf_blend(&a, &b, cases[i].weight);
        const quatf_t rotation = quatf_nlerp(a.rotation, b.rotation, cases[i].weight);
        assert_blend_vec3(cases[i].position, got.position);
        assert_blend_vec3(cases[i].scale, got.scale);
        TEST_ASSERT_EQUAL_MEMORY(&rotation, &got.rotation, sizeof rotation);
        TEST_ASSERT_FALSE(got.cached);
    }
    const transformf_t before = transformf_blend(&a, &b, -1.0F);
    const transformf_t after = transformf_blend(&a, &b, 2.0F);
    TEST_ASSERT_EQUAL_MEMORY(&a.position, &before.position, sizeof a.position);
    TEST_ASSERT_EQUAL_MEMORY(&a.scale, &before.scale, sizeof a.scale);
    TEST_ASSERT_EQUAL_MEMORY(&b.position, &after.position, sizeof b.position);
    TEST_ASSERT_EQUAL_MEMORY(&b.scale, &after.scale, sizeof b.scale);
    TEST_ASSERT_FALSE(before.cached);
    TEST_ASSERT_FALSE(after.cached);
}

void
suite_transform_decompose(void) {
    RUN_TEST(test_inverse_point_under_an_uneven_parent);
    RUN_TEST(test_blend_clamps_and_interpolates_trs);
    RUN_TEST(test_relative_undoes_compose_under_an_even_parent);
    RUN_TEST(test_trs_matrix_decomposition_and_refusals);
    RUN_TEST(test_compose_under_an_evenly_scaled_parent_is_the_matrix_product);
    RUN_TEST(test_compose_under_an_unevenly_scaled_parent_keeps_the_position);
}

SUITE_REGISTER(suite_transform_decompose);
