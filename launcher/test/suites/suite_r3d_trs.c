/*
 * Portable suite: render/r3d_trs.h, a float translation, quaternion and
 * scale as one matrix4i transform. The rotation is judged by the matrix
 * matrix4i builds from it, since a rotation has more than one Euler
 * triple. The quaternions pinned below are those of two known angle triples,
 * worked out from matrix4i's own convention.
 */

#include <math.h>
#include <stdint.h>

#include "suites.h"
#include "unity.h"

#include "render/r3d_trs.h"

/* Angles land on whole fixed-point units and matrix4i rounds each product of
 * sines, so an equal rotation reached by another triple differs by a few
 * units; a wrong convention is off by hundreds. */
#define MATRIX_SLACK 24

static void
matrix_of(vec4i_t rotation, matrix4i_t m) {
    matrix4i_rotation(rotation.x, rotation.y, rotation.z, m);
}

static void
assert_same_rotation(vec4i_t want, vec4i_t got) {
    matrix4i_t a;
    matrix4i_t b;
    matrix_of(want, a);
    matrix_of(got, b);
    for (int r = 0; r < 3; r++) {
        for (int c = 0; c < 3; c++) {
            TEST_ASSERT_INT_WITHIN(MATRIX_SLACK, a[r][c], b[r][c]);
        }
    }
}

/* The unit quaternion of a rotation matrix acting on column vectors, whose
 * entry (r, c) is matrix4i's m[r][c]. */
static void
quaternion_of(matrix4i_t m, float q[4]) {
    float e[3][3];
    for (int r = 0; r < 3; r++) {
        for (int c = 0; c < 3; c++) {
            e[r][c] = (float)m[r][c] / (float)VEC4I_ONE;
        }
    }
    const float trace = e[0][0] + e[1][1] + e[2][2];
    if (trace > 0.0F) {
        const float k = sqrtf(trace + 1.0F) * 2.0F;
        q[0] = (e[2][1] - e[1][2]) / k;
        q[1] = (e[0][2] - e[2][0]) / k;
        q[2] = (e[1][0] - e[0][1]) / k;
        q[3] = k / 4.0F;
    } else if (e[0][0] > e[1][1] && e[0][0] > e[2][2]) {
        const float k = sqrtf(1.0F + e[0][0] - e[1][1] - e[2][2]) * 2.0F;
        q[0] = k / 4.0F;
        q[1] = (e[0][1] + e[1][0]) / k;
        q[2] = (e[0][2] + e[2][0]) / k;
        q[3] = (e[2][1] - e[1][2]) / k;
    } else if (e[1][1] > e[2][2]) {
        const float k = sqrtf(1.0F + e[1][1] - e[0][0] - e[2][2]) * 2.0F;
        q[0] = (e[0][1] + e[1][0]) / k;
        q[1] = k / 4.0F;
        q[2] = (e[1][2] + e[2][1]) / k;
        q[3] = (e[0][2] - e[2][0]) / k;
    } else {
        const float k = sqrtf(1.0F + e[2][2] - e[0][0] - e[1][1]) * 2.0F;
        q[0] = (e[0][2] + e[2][0]) / k;
        q[1] = (e[1][2] + e[2][1]) / k;
        q[2] = k / 4.0F;
        q[3] = (e[1][0] - e[0][1]) / k;
    }
    const float length = sqrtf((q[0] * q[0]) + (q[1] * q[1]) + (q[2] * q[2]) + (q[3] * q[3]));
    for (int i = 0; i < 4; i++) {
        q[i] /= length;
    }
}

static const float ORIGIN[3] = {0.0F, 0.0F, 0.0F};
static const float UNIT[3] = {1.0F, 1.0F, 1.0F};

static void
test_translation_and_scale_land_in_vec4i_units(void) {
    const float move[3] = {1.5F, -2.0F, 0.25F};
    const float size[3] = {1.0F, 0.6F, 2.0F};
    const float identity[4] = {0.0F, 0.0F, 0.0F, 1.0F};
    const matrix4i_transform_t t = r3d_trs_to_transform(move, identity, size);
    TEST_ASSERT_EQUAL_INT32(VEC4I_ONE * 3 / 2, t.translation.x);
    TEST_ASSERT_EQUAL_INT32(-2 * VEC4I_ONE, t.translation.y);
    TEST_ASSERT_EQUAL_INT32(VEC4I_ONE / 4, t.translation.z);
    TEST_ASSERT_EQUAL_INT32(VEC4I_ONE, t.scale.x);
    TEST_ASSERT_EQUAL_INT32((VEC4I_ONE * 6 + 5) / 10, t.scale.y);
    TEST_ASSERT_EQUAL_INT32(2 * VEC4I_ONE, t.scale.z);
    TEST_ASSERT_EQUAL_INT32(0, t.rotation.x);
    TEST_ASSERT_EQUAL_INT32(0, t.rotation.y);
    TEST_ASSERT_EQUAL_INT32(0, t.rotation.z);
}

static void
test_a_rotation_read_back_builds_the_matrix_it_came_from(void) {
    /* Whole fixed-point units, x kept clear of a quarter turn where Euler angles
     * degenerate and matrix4i's rounded matrix stops being a rotation. */
    for (int x = -75; x <= 75; x += 25) {
        for (int y = -256; y <= 256; y += 64) {
            for (int z = -256; z <= 256; z += 64) {
                const vec4i_t want = {x, y, z, 0};
                matrix4i_t m;
                matrix_of(want, m);
                float q[4];
                quaternion_of(m, q);
                const matrix4i_transform_t t = r3d_trs_to_transform(ORIGIN, q, UNIT);
                assert_same_rotation(want, t.rotation);
            }
        }
    }
}

static void
test_the_quaternions_of_known_angles_read_back_as_those_angles(void) {
    /* -135, -45, 90 degrees and -180, -45, 0, as fixed-point units (512 to a turn). */
    const float first[4] = {0.5F, 0.70710678F, -0.5F, 0.0F};
    const float second[4] = {0.92387953F, 0.0F, -0.38268343F, 0.0F};
    assert_same_rotation((vec4i_t){-192, -64, 128, 0}, r3d_trs_to_transform(ORIGIN, first, UNIT).rotation);
    assert_same_rotation((vec4i_t){-256, -64, 0, 0}, r3d_trs_to_transform(ORIGIN, second, UNIT).rotation);
}

void
suite_r3d_trs(void) {
    RUN_TEST(test_translation_and_scale_land_in_vec4i_units);
    RUN_TEST(test_a_rotation_read_back_builds_the_matrix_it_came_from);
    RUN_TEST(test_the_quaternions_of_known_angles_read_back_as_those_angles);
}

SUITE_REGISTER(suite_r3d_trs);
