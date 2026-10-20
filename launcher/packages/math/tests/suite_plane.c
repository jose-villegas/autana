#include <math.h>
#include <string.h>

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
static const projectf_t PLANE_LENS = {1.0F, -1.0F, 0.25F, -0.5F, 1.0F};
static const projectf_t PLANE_SCALED_LENS = {2.0F, 3.0F, 0.25F, -0.5F, 1.0F};
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
test_fixed_normalize_extreme_normals_and_distance_saturation(void) {
    static const planex_t cases[] = {{{1, 0, 0}, 1},
                                     {{30000 * MATHX_ONE, 0, 0}, 30000 * MATHX_ONE},
                                     {{1, 0, 0}, MATHX_ONE},
                                     {{1, 0, 0}, -MATHX_ONE}};
    static const int32_t distances[] = {MATHX_ONE, MATHX_ONE, INT32_MAX, INT32_MIN};
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        const planex_t out = planex_normalize(cases[i]);
        TEST_ASSERT_EQUAL_INT32(MATHX_ONE, out.normal.x);
        TEST_ASSERT_EQUAL_INT32(0, out.normal.y);
        TEST_ASSERT_EQUAL_INT32(0, out.normal.z);
        TEST_ASSERT_EQUAL_INT32(distances[i], out.d);
    }
}

static void
test_fixed_normalize_zero_and_tilted_normals(void) {
    static const int32_t distances[] = {INT32_MIN, -1, 0, 1, INT32_MAX};
    for (size_t i = 0; i < sizeof(distances) / sizeof(distances[0]); i++) {
        const planex_t out = planex_normalize((planex_t){{0, 0, 0}, distances[i]});
        TEST_ASSERT_TRUE(vec3x_equal((vec3x_t){0, 0, 0}, out.normal));
        TEST_ASSERT_EQUAL_INT32(mathx_div(distances[i], 0), out.d);
    }
    static const planex_t cases[] = {{{-3, 4, 12}, -7},
                                     {{-3 * MATHX_ONE, 4 * MATHX_ONE, 12 * MATHX_ONE}, 7 * MATHX_ONE},
                                     {{-3 * (INT32_MAX / 12), 4 * (INT32_MAX / 12), 12 * (INT32_MAX / 12)}, INT32_MIN}};
    const double component_error = 0.5 + 0x1p-12;
    const double quotient_relative_error = 0x1p-28;
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        const planex_t p = cases[i];
        const double length =
            sqrt((double)p.normal.x * p.normal.x + (double)p.normal.y * p.normal.y + (double)p.normal.z * p.normal.z);
        const planex_t out = planex_normalize(p);
        TEST_ASSERT_TRUE(fabs(out.normal.x - (double)p.normal.x * MATHX_ONE / length) <= component_error);
        TEST_ASSERT_TRUE(fabs(out.normal.y - (double)p.normal.y * MATHX_ONE / length) <= component_error);
        TEST_ASSERT_TRUE(fabs(out.normal.z - (double)p.normal.z * MATHX_ONE / length) <= component_error);
        const double exact = (double)p.d * MATHX_ONE / length;
        TEST_ASSERT_TRUE(fabs(out.d - exact) <= 0.5 + fabs(exact) * quotient_relative_error);
    }
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
test_from_rows_tests_the_mapped_point(void) {
    const planef_t target = planef_from_normal_point(PLANE_OBLIQUE_NORMAL, PLANE_OBLIQUE_POINT);
    const planef_t source = planef_from_rows(&PLANE_MAP, target.normal.x, target.normal.y, target.normal.z, target.d);
    const vec3f_t points[] = {PLANE_OBLIQUE_POINT, PLANE_OBLIQUE_QUERY, PLANE_ORIGIN, PLANE_AXIS_Y};
    for (size_t i = 0; i < sizeof(points) / sizeof(points[0]); i++) {
        TEST_ASSERT_FLOAT_WITHIN(PLANE_FLOAT_TOLERANCE, planef_distance(target, mat4f_apply(&PLANE_MAP, points[i])),
                                 planef_distance(source, points[i]));
    }
}

/* The frustum built through the identity and through PLANE_MAP, in both number types. */
typedef struct {
    planef_t planes[PLANE_FRUSTUM_COUNT], mapped[PLANE_FRUSTUM_COUNT];
    planex_t fixed[PLANE_FRUSTUM_COUNT], fixed_mapped[PLANE_FRUSTUM_COUNT];
} frustum_fixture_t;

static mat4x_t
fixed_matrix(const mat4f_t* m) {
    mat4x_t out;
    for (int r = 0; r < 4; r++) {
        for (int c = 0; c < 4; c++) {
            out.m[r][c] = mathf_to_x(m->m[r][c]);
        }
    }
    return out;
}

static void
frustum_fixture(frustum_fixture_t* f, const projectf_t* lens) {
    const mat4f_t identity = mat4f_identity();
    const mat4x_t fixed_identity = mat4x_identity();
    const mat4x_t fixed_map = fixed_matrix(&PLANE_MAP);
    const rectx_t screen = {mathf_to_x(PLANE_SCREEN.x), mathf_to_x(PLANE_SCREEN.y), mathf_to_x(PLANE_SCREEN.w),
                            mathf_to_x(PLANE_SCREEN.h)};
    const projectx_t fixed_lens = {mathf_to_x(lens->fx), mathf_to_x(lens->fy), mathf_to_x(lens->cx),
                                   mathf_to_x(lens->cy), mathf_to_x(lens->near_z)};
    planef_frustum(&identity, lens, PLANE_SCREEN, f->planes);
    planef_frustum(&PLANE_MAP, lens, PLANE_SCREEN, f->mapped);
    planex_frustum(&fixed_identity, &fixed_lens, screen, f->fixed);
    planex_frustum(&fixed_map, &fixed_lens, screen, f->fixed_mapped);
}

/* The screen edge a side plane passes through: left, right, top, bottom. */
static float
frustum_edge(int edge) {
    const float edges[] = {PLANE_SCREEN.x, rectf_right(PLANE_SCREEN), PLANE_SCREEN.y, rectf_bottom(PLANE_SCREEN)};
    return edges[edge - PLANE_FRUSTUM_LEFT];
}

/* A point at PLANE_DEPTH whose screen position is PLANE_EDGE_STEP inside (side 1),
 * on (0) or outside (-1) one side plane's edge, centred on the other axis. */
static vec3f_t
edge_point(const projectf_t* lens, int edge, int side) {
    const bool low_edge = edge == PLANE_FRUSTUM_LEFT || edge == PLANE_FRUSTUM_TOP;
    const float inward = low_edge ? mathf_one() : -mathf_one();
    const float screen = frustum_edge(edge) + ((float)side * PLANE_EDGE_STEP * inward);
    vec3f_t q = {0.0F, 0.0F, PLANE_DEPTH};
    if (edge <= PLANE_FRUSTUM_RIGHT) {
        q.x = (screen - lens->cx) * q.z / lens->fx;
    } else {
        q.y = (lens->cy - screen) * q.z / lens->fy;
    }
    return q;
}

/* Through the identity, q is outside only the plane whose edge it crossed (on it at side 0);
 * through PLANE_MAP, each plane tests q as the identity-built one tests the mapped point. */
static void
assert_edge_point(const frustum_fixture_t* f, const projectf_t* lens, int edge, int side) {
    const vec3f_t q = edge_point(lens, edge, side);
    const vec2f_t screen = projectf_screen(lens, q);
    const float coordinate = edge <= PLANE_FRUSTUM_RIGHT ? screen.x : screen.y;
    const float inward = edge == PLANE_FRUSTUM_LEFT || edge == PLANE_FRUSTUM_TOP ? 1.0F : -1.0F;
    TEST_ASSERT_FLOAT_WITHIN(PLANE_FLOAT_TOLERANCE, (float)side * PLANE_EDGE_STEP,
                             (coordinate - frustum_edge(edge)) * inward);
    const vec3f_t mapped_q = mat4f_apply(&PLANE_MAP, q);
    for (int p = 0; p < PLANE_FRUSTUM_COUNT; p++) {
        const float d = planef_distance(f->planes[p], q);
        if (p == edge && side == 0) {
            TEST_ASSERT_FLOAT_WITHIN(PLANE_FLOAT_TOLERANCE, 0.0F, d);
        } else {
            TEST_ASSERT_EQUAL(p != edge || side > 0, d >= 0.0F);
        }
        const float mapped_distance = planef_distance(f->mapped[p], q);
        const float camera_distance = planef_distance(f->planes[p], mapped_q);
        TEST_ASSERT_FLOAT_WITHIN(PLANE_FLOAT_TOLERANCE, camera_distance, mapped_distance);
        TEST_ASSERT_EQUAL(camera_distance >= 0.0F, mapped_distance >= 0.0F);
        assert_fixed_plane(f->planes[p], f->fixed[p], q);
        assert_fixed_plane(f->mapped[p], f->fixed_mapped[p], q);
    }
}

static void
test_frustum_edge_points_directly_and_through_a_map(void) {
    frustum_fixture_t f;
    const projectf_t lenses[] = {PLANE_LENS, PLANE_SCALED_LENS};
    for (size_t i = 0; i < sizeof(lenses) / sizeof(lenses[0]); i++) {
        frustum_fixture(&f, &lenses[i]);
        for (int edge = PLANE_FRUSTUM_LEFT; edge < PLANE_FRUSTUM_COUNT; edge++) {
            for (int side = -1; side <= 1; side++) {
                assert_edge_point(&f, &lenses[i], edge, side);
            }
        }
    }
}

static void
test_frustum_matches_hand_written_planes_bitwise(void) {
    planef_t actual[PLANE_FRUSTUM_COUNT];
    const planef_t expected[PLANE_FRUSTUM_COUNT] = {
        [PLANE_FRUSTUM_NEAR] = planef_from_rows(&PLANE_MAP, 0.0F, 0.0F, 1.0F, -PLANE_LENS.near_z),
        [PLANE_FRUSTUM_LEFT] = planef_from_rows(&PLANE_MAP, 1.0F, 0.0F, PLANE_LENS.cx - PLANE_SCREEN.x, 0.0F),
        [PLANE_FRUSTUM_RIGHT] =
            planef_from_rows(&PLANE_MAP, -1.0F, 0.0F, rectf_right(PLANE_SCREEN) - PLANE_LENS.cx, 0.0F),
        [PLANE_FRUSTUM_TOP] = planef_from_rows(&PLANE_MAP, 0.0F, 1.0F, PLANE_LENS.cy - PLANE_SCREEN.y, 0.0F),
        [PLANE_FRUSTUM_BOTTOM] =
            planef_from_rows(&PLANE_MAP, 0.0F, -1.0F, rectf_bottom(PLANE_SCREEN) - PLANE_LENS.cy, 0.0F),
    };
    planef_frustum(&PLANE_MAP, &PLANE_LENS, PLANE_SCREEN, actual);
    for (int p = 0; p < PLANE_FRUSTUM_COUNT; p++) {
        TEST_ASSERT_EQUAL_INT(0, memcmp(&expected[p], &actual[p], sizeof(expected[p])));
    }
}

static void
test_frustum_near_plane_sits_at_near_z(void) {
    frustum_fixture_t f;
    frustum_fixture(&f, &PLANE_LENS);
    for (int side = -1; side <= 1; side++) {
        const float offset = (float)side * PLANE_EDGE_STEP;
        const vec3f_t q = {0.0F, 0.0F, PLANE_NEAR + offset};
        TEST_ASSERT_FLOAT_WITHIN(PLANE_FLOAT_TOLERANCE, offset, planef_distance(f.planes[PLANE_FRUSTUM_NEAR], q));
        for (int p = 0; p < PLANE_FRUSTUM_COUNT; p++) {
            if (p != PLANE_FRUSTUM_NEAR) {
                TEST_ASSERT_TRUE(planef_distance(f.planes[p], q) >= 0.0F);
            }
            assert_fixed_plane(f.planes[p], f.fixed[p], q);
        }
    }
}

static void
test_frustum_corner_rays_lie_on_both_side_planes(void) {
    frustum_fixture_t f;
    const projectf_t lenses[] = {PLANE_LENS, PLANE_SCALED_LENS};
    for (size_t i = 0; i < sizeof(lenses) / sizeof(lenses[0]); i++) {
        frustum_fixture(&f, &lenses[i]);
        for (int x = PLANE_FRUSTUM_LEFT; x <= PLANE_FRUSTUM_RIGHT; x++) {
            for (int y = PLANE_FRUSTUM_TOP; y <= PLANE_FRUSTUM_BOTTOM; y++) {
                const vec3f_t corner = {(frustum_edge(x) - lenses[i].cx) * PLANE_DEPTH / lenses[i].fx,
                                        (lenses[i].cy - frustum_edge(y)) * PLANE_DEPTH / lenses[i].fy, PLANE_DEPTH};
                TEST_ASSERT_FLOAT_WITHIN(PLANE_FLOAT_TOLERANCE, 0.0F, planef_distance(f.planes[x], corner));
                TEST_ASSERT_FLOAT_WITHIN(PLANE_FLOAT_TOLERANCE, 0.0F, planef_distance(f.planes[y], corner));
                for (int p = 0; p < PLANE_FRUSTUM_COUNT; p++) {
                    assert_fixed_plane(f.planes[p], f.fixed[p], corner);
                }
            }
        }
    }
}

void
suite_plane(void) {
    RUN_TEST(test_fixed_normalize_zero_and_tilted_normals);
    RUN_TEST(test_fixed_normalize_extreme_normals_and_distance_saturation);
    RUN_TEST(test_construction_winding_distance_and_normalize);
    RUN_TEST(test_oblique_normalization_scales_normal_and_distance);
    RUN_TEST(test_from_rows_tests_the_mapped_point);
    RUN_TEST(test_frustum_edge_points_directly_and_through_a_map);
    RUN_TEST(test_frustum_matches_hand_written_planes_bitwise);
    RUN_TEST(test_frustum_near_plane_sits_at_near_z);
    RUN_TEST(test_frustum_corner_rays_lie_on_both_side_planes);
}

SUITE_REGISTER(suite_plane);
