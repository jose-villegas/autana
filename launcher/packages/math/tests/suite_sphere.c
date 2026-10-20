#include "math/geometry/spheref.h"
#include "suites.h"
#include "unity.h"

static const float SPHERE_TOLERANCE = 0.00001F;
static const spheref_t SPHERE_UNIT = {{0, 0, 0}, 1};
static const vec3f_t SPHERE_BOUNDARY = {1, 0, 0};
static const vec3f_t SPHERE_FAR = {3, 0, 0};
static const box3f_t SPHERE_BOX = {{-1, -1, -1}, {1, 1, 1}};
static const box3f_t SPHERE_FLAT_BOX = {{-2, -3, 2}, {4, 5, 2}};
static const spheref_t SPHERE_FLAT_ENCLOSING = {{1, 1, 2}, 5};
static const float SPHERE_ENTRY = 1.0F / 3.0F;

static void
test_sphere_closed_queries(void) {
    const spheref_t enclosing = spheref_from_box(SPHERE_FLAT_BOX);
    TEST_ASSERT_TRUE(vec3f_equal(SPHERE_FLAT_ENCLOSING.centre, enclosing.centre));
    TEST_ASSERT_FLOAT_WITHIN(SPHERE_TOLERANCE, SPHERE_FLAT_ENCLOSING.radius, enclosing.radius);
    TEST_ASSERT_TRUE(spheref_contains_point(SPHERE_UNIT, SPHERE_UNIT.centre));
    TEST_ASSERT_TRUE(spheref_contains_point(SPHERE_UNIT, SPHERE_BOUNDARY));
    TEST_ASSERT_FALSE(spheref_contains_point(SPHERE_UNIT, SPHERE_FAR));
    TEST_ASSERT_TRUE(spheref_overlaps(SPHERE_UNIT, (spheref_t){SPHERE_BOUNDARY, SPHERE_UNIT.radius}));
    TEST_ASSERT_TRUE(
        spheref_overlaps(SPHERE_UNIT, (spheref_t){vec3f_scale(SPHERE_BOUNDARY, mathf_two()), SPHERE_UNIT.radius}));
    TEST_ASSERT_FALSE(spheref_overlaps(SPHERE_UNIT, (spheref_t){SPHERE_FAR, SPHERE_UNIT.radius}));
    TEST_ASSERT_TRUE(spheref_overlaps_box(SPHERE_UNIT, SPHERE_BOX));
    TEST_ASSERT_TRUE(
        spheref_overlaps_box((spheref_t){vec3f_scale(SPHERE_BOUNDARY, mathf_two()), SPHERE_UNIT.radius}, SPHERE_BOX));
    TEST_ASSERT_FALSE(spheref_overlaps_box((spheref_t){SPHERE_FAR, SPHERE_UNIT.radius}, SPHERE_BOX));
    TEST_ASSERT_TRUE(spheref_contains_point((spheref_t){SPHERE_BOUNDARY, 0}, SPHERE_BOUNDARY));
}

static void
test_sphere_segment_roots_and_tangency(void) {
    float t = -mathf_one();
    const vec3f_t start = vec3f_scale(SPHERE_FAR, -mathf_one());
    TEST_ASSERT_TRUE(spheref_segment_hit(SPHERE_UNIT, start, SPHERE_FAR, &t));
    TEST_ASSERT_FLOAT_WITHIN(SPHERE_TOLERANCE, SPHERE_ENTRY, t);
    TEST_ASSERT_TRUE(spheref_segment_hit(SPHERE_UNIT, SPHERE_UNIT.centre, SPHERE_FAR, &t));
    TEST_ASSERT_EQUAL_FLOAT(mathf_zero(), t);
    TEST_ASSERT_TRUE(spheref_segment_hit(SPHERE_UNIT, SPHERE_UNIT.centre, SPHERE_UNIT.centre, &t));
    TEST_ASSERT_FALSE(spheref_segment_hit(SPHERE_UNIT, SPHERE_FAR, SPHERE_FAR, &t));
    TEST_ASSERT_FALSE(spheref_segment_hit(SPHERE_UNIT, SPHERE_FAR, vec3f_scale(SPHERE_FAR, mathf_two()), &t));
    TEST_ASSERT_TRUE(spheref_segment_hit(SPHERE_UNIT, SPHERE_FAR, SPHERE_BOUNDARY, &t));
    TEST_ASSERT_EQUAL_FLOAT(mathf_one(), t);
    TEST_ASSERT_TRUE(spheref_segment_hit(SPHERE_UNIT, (vec3f_t){start.x, SPHERE_UNIT.radius, 0},
                                         (vec3f_t){SPHERE_FAR.x, SPHERE_UNIT.radius, 0}, &t));
    TEST_ASSERT_FLOAT_WITHIN(SPHERE_TOLERANCE, (mathf_one() / mathf_two()), t);
    TEST_ASSERT_FALSE(spheref_segment_hit(SPHERE_UNIT, (vec3f_t){start.x, SPHERE_FAR.x, 0},
                                          (vec3f_t){SPHERE_FAR.x, SPHERE_FAR.x, 0}, &t));
}

void
suite_sphere(void) {
    RUN_TEST(test_sphere_closed_queries);
    RUN_TEST(test_sphere_segment_roots_and_tangency);
}

SUITE_REGISTER(suite_sphere);
