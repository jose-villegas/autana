#include "math/linear/transformf.h"
#include "suites.h"
#include "unity.h"

static void
test_trs_matrix_decomposition_and_refusals(void) {
    const vec3f_t position = {3, 4, 5}, scale = {2, 3, 4};
    const quatf_t rotation = quatf_from_axis_angle((vec3f_t){0, 1, 0}, 0.7F);
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

void
suite_transform_decompose(void) {
    RUN_TEST(test_trs_matrix_decomposition_and_refusals);
}

SUITE_REGISTER(suite_transform_decompose);
