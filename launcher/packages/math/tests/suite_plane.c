#include "suites.h"
#include "unity.h"

#include "math/geometry/planef.h"
#include "math/geometry/planex.h"
#include "math/linear/vec_convert.h"

static const float PLANE_FLOAT_TOLERANCE = 0.00001F;
static const float PLANE_FIXED_TOLERANCE = 32.0F / MATHX_ONE;
static const float PLANE_NEAR = 1.0F;
static const float PLANE_DEPTH = 2.0F;
static const float PLANE_EDGE_STEP = 0.25F;
static const float PLANE_NORMAL_SCALE = 2.0F;
static const vec3f_t PLANE_OBLIQUE_NORMAL = {3.0F, 4.0F, 0.0F};
static const vec3f_t PLANE_OBLIQUE_POINT = {0.5F, -0.25F, 1.0F};
static const vec3f_t PLANE_OBLIQUE_QUERY = {-0.25F, 0.75F, 2.0F};
static const float PLANE_OBLIQUE_LENGTH = 5.0F;
static const vec2f_t PLANE_CENTER = {0.25F, -0.5F};
static const rectf_t PLANE_SCREEN = {-2.0F, -1.5F, 4.0F, 3.0F};
static const vec3f_t PLANE_ORIGIN = {0.0F, 0.0F, 1.0F};
static const vec3f_t PLANE_AXIS_X = {1.0F, 0.0F, 1.0F};
static const vec3f_t PLANE_AXIS_Y = {0.0F, 2.0F, 1.0F};
static const vec3f_t PLANE_NORMAL = {0.0F, 0.0F, 2.0F};
static const mat4f_t PLANE_MAP = {
    {{0.0F, -2.0F, 0.0F, 0.5F}, {0.5F, 0.0F, 0.0F, -0.25F}, {0.0F, 0.0F, 1.5F, 0.5F}, {0.0F, 0.0F, 0.0F, 1.0F}}};

static void
assert_fixed_plane(planef_t f, planex_t x, vec3f_t point) {
    TEST_ASSERT_FLOAT_WITHIN(PLANE_FIXED_TOLERANCE, f.normal.x, mathx_to_f(x.normal.x));
    TEST_ASSERT_FLOAT_WITHIN(PLANE_FIXED_TOLERANCE, f.normal.y, mathx_to_f(x.normal.y));
    TEST_ASSERT_FLOAT_WITHIN(PLANE_FIXED_TOLERANCE, f.normal.z, mathx_to_f(x.normal.z));
    TEST_ASSERT_FLOAT_WITHIN(PLANE_FIXED_TOLERANCE, f.d, mathx_to_f(x.d));
    TEST_ASSERT_FLOAT_WITHIN(PLANE_FIXED_TOLERANCE, planef_distance(f, point),
                             mathx_to_f(planex_distance(x, vec3x_from_vec3f(point))));
}

static void
test_construction_winding_distance_and_normalize(void) {
    const planef_t normal = planef_from_normal_point(PLANE_NORMAL, PLANE_ORIGIN);
    const planef_t points = planef_from_points(PLANE_ORIGIN, PLANE_AXIS_X, PLANE_AXIS_Y);
    const planef_t reversed = planef_from_points(PLANE_ORIGIN, PLANE_AXIS_Y, PLANE_AXIS_X);
    const planex_t fixed_normal =
        planex_from_normal_point(vec3x_from_vec3f(PLANE_NORMAL), vec3x_from_vec3f(PLANE_ORIGIN));
    const planex_t fixed_points = planex_from_points(vec3x_from_vec3f(PLANE_ORIGIN), vec3x_from_vec3f(PLANE_AXIS_X),
                                                     vec3x_from_vec3f(PLANE_AXIS_Y));
    const planex_t fixed_reversed = planex_from_points(vec3x_from_vec3f(PLANE_ORIGIN), vec3x_from_vec3f(PLANE_AXIS_Y),
                                                       vec3x_from_vec3f(PLANE_AXIS_X));
    const planef_t unit = planef_normalize(normal);
    const planex_t fixed_unit = planex_normalize(fixed_normal);
    TEST_ASSERT_FLOAT_WITHIN(PLANE_FLOAT_TOLERANCE, mathf_one(), vec3f_dot(unit.normal, unit.normal));
    for (int side = -1; side <= 1; side++) {
        const vec3f_t q = {PLANE_ORIGIN.x, PLANE_ORIGIN.y, PLANE_ORIGIN.z + (float)side * PLANE_EDGE_STEP};
        const float distance = (float)side * PLANE_EDGE_STEP * PLANE_NORMAL_SCALE;
        TEST_ASSERT_FLOAT_WITHIN(PLANE_FLOAT_TOLERANCE, distance, planef_distance(normal, q));
        TEST_ASSERT_FLOAT_WITHIN(PLANE_FLOAT_TOLERANCE, distance, planef_distance(points, q));
        TEST_ASSERT_FLOAT_WITHIN(PLANE_FLOAT_TOLERANCE, -distance, planef_distance(reversed, q));
        TEST_ASSERT_FLOAT_WITHIN(PLANE_FLOAT_TOLERANCE, distance / PLANE_NORMAL_SCALE, planef_distance(unit, q));
        assert_fixed_plane(normal, fixed_normal, q);
        assert_fixed_plane(points, fixed_points, q);
        assert_fixed_plane(reversed, fixed_reversed, q);
        assert_fixed_plane(unit, fixed_unit, q);
    }
}

static void
test_oblique_normalization_scales_normal_and_distance(void) {
    const planef_t plane = planef_from_normal_point(PLANE_OBLIQUE_NORMAL, PLANE_OBLIQUE_POINT);
    const planex_t fixed =
        planex_from_normal_point(vec3x_from_vec3f(PLANE_OBLIQUE_NORMAL), vec3x_from_vec3f(PLANE_OBLIQUE_POINT));
    const planef_t unit = planef_normalize(plane);
    const planex_t fixed_unit = planex_normalize(fixed);
    TEST_ASSERT_FLOAT_WITHIN(PLANE_FLOAT_TOLERANCE, mathf_one(), vec3f_dot(unit.normal, unit.normal));
    TEST_ASSERT_FLOAT_WITHIN(PLANE_FLOAT_TOLERANCE, planef_distance(plane, PLANE_OBLIQUE_QUERY) / PLANE_OBLIQUE_LENGTH,
                             planef_distance(unit, PLANE_OBLIQUE_QUERY));
    assert_fixed_plane(plane, fixed, PLANE_OBLIQUE_QUERY);
    assert_fixed_plane(unit, fixed_unit, PLANE_OBLIQUE_QUERY);
}

static void
test_frustum_edges_and_affine_source_space(void) {
    const mat4f_t identity = mat4f_identity();
    planef_t planes[PLANE_FRUSTUM_COUNT], mapped[PLANE_FRUSTUM_COUNT];
    planex_t fixed[PLANE_FRUSTUM_COUNT], fixed_mapped[PLANE_FRUSTUM_COUNT];
    const mat4x_t fixed_identity = mat4x_identity();
    mat4x_t fixed_map;
    for (int r = 0; r < 4; r++) {
        for (int c = 0; c < 4; c++) {
            fixed_map.m[r][c] = mathf_to_x(PLANE_MAP.m[r][c]);
        }
    }
    const rectx_t screen = {mathf_to_x(PLANE_SCREEN.x), mathf_to_x(PLANE_SCREEN.y), mathf_to_x(PLANE_SCREEN.w),
                            mathf_to_x(PLANE_SCREEN.h)};
    planef_frustum(&identity, PLANE_NEAR, PLANE_CENTER, PLANE_SCREEN, planes);
    planef_frustum(&PLANE_MAP, PLANE_NEAR, PLANE_CENTER, PLANE_SCREEN, mapped);
    planex_frustum(&fixed_identity, mathf_to_x(PLANE_NEAR), vec2x_from_vec2f(PLANE_CENTER), screen, fixed);
    planex_frustum(&fixed_map, mathf_to_x(PLANE_NEAR), vec2x_from_vec2f(PLANE_CENTER), screen, fixed_mapped);
    const float edges[] = {PLANE_SCREEN.x, rectf_right(PLANE_SCREEN), PLANE_SCREEN.y, rectf_bottom(PLANE_SCREEN)};
    for (int edge = PLANE_FRUSTUM_LEFT; edge < PLANE_FRUSTUM_COUNT; edge++) {
        for (int side = -1; side <= 1; side++) {
            vec3f_t q = {0.0F, 0.0F, PLANE_DEPTH};
            const float direction =
                (edge == PLANE_FRUSTUM_LEFT || edge == PLANE_FRUSTUM_TOP) ? mathf_one() : -mathf_one();
            const float coordinate = edges[edge - PLANE_FRUSTUM_LEFT] + (float)side * PLANE_EDGE_STEP * direction;
            if (edge <= PLANE_FRUSTUM_RIGHT) {
                q.x = (coordinate - PLANE_CENTER.x) * q.z;
            } else {
                q.y = (coordinate - PLANE_CENTER.y) * q.z;
            }
            for (int p = 0; p < PLANE_FRUSTUM_COUNT; p++) {
                const float d = planef_distance(planes[p], q);
                if (p == edge && side == 0) {
                    TEST_ASSERT_FLOAT_WITHIN(PLANE_FLOAT_TOLERANCE, 0.0F, d);
                } else {
                    TEST_ASSERT_EQUAL(p != edge || side > 0, d >= 0.0F);
                }
                assert_fixed_plane(planes[p], fixed[p], q);
                const float mapped_distance = planef_distance(mapped[p], q);
                const float camera_distance = planef_distance(planes[p], mat4f_apply(&PLANE_MAP, q));
                TEST_ASSERT_FLOAT_WITHIN(PLANE_FLOAT_TOLERANCE, camera_distance, mapped_distance);
                TEST_ASSERT_EQUAL(camera_distance >= 0.0F, mapped_distance >= 0.0F);
                assert_fixed_plane(mapped[p], fixed_mapped[p], q);
            }
        }
    }
    const vec3f_t behind = {0.0F, 0.0F, PLANE_NEAR - PLANE_EDGE_STEP};
    for (int p = 0; p < PLANE_FRUSTUM_COUNT; p++) {
        TEST_ASSERT_EQUAL(p != PLANE_FRUSTUM_NEAR, planef_distance(planes[p], behind) >= 0.0F);
        assert_fixed_plane(planes[p], fixed[p], behind);
    }
    for (int x = 0; x < 2; x++) {
        for (int y = 0; y < 2; y++) {
            const vec3f_t corner = {(edges[x] - PLANE_CENTER.x) * PLANE_DEPTH,
                                    (edges[2 + y] - PLANE_CENTER.y) * PLANE_DEPTH, PLANE_DEPTH};
            TEST_ASSERT_FLOAT_WITHIN(PLANE_FLOAT_TOLERANCE, 0.0F,
                                     planef_distance(planes[PLANE_FRUSTUM_LEFT + x], corner));
            TEST_ASSERT_FLOAT_WITHIN(PLANE_FLOAT_TOLERANCE, 0.0F,
                                     planef_distance(planes[PLANE_FRUSTUM_TOP + y], corner));
            for (int p = 0; p < PLANE_FRUSTUM_COUNT; p++) {
                assert_fixed_plane(planes[p], fixed[p], corner);
            }
        }
    }
}

void
suite_plane(void) {
    RUN_TEST(test_construction_winding_distance_and_normalize);
    RUN_TEST(test_oblique_normalization_scales_normal_and_distance);
    RUN_TEST(test_frustum_edges_and_affine_source_space);
}

SUITE_REGISTER(suite_plane);
