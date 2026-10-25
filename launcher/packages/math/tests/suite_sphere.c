#include "math/geometry/spheref.h"
#include "testkit/suite.h"
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
test_sphere_closest_point(void) {
    static const spheref_t translated = {{1, 1, 3}, 7};
    static const vec3f_t outside = {5, 7, 15};
    static const vec3f_t boundary = {3, 4, 9};
    TEST_ASSERT_TRUE(vec3f_equal(SPHERE_UNIT.centre, spheref_closest_point(SPHERE_UNIT, SPHERE_UNIT.centre)));
    const vec3f_t inside = vec3f_scale(SPHERE_BOUNDARY, mathf_half());
    TEST_ASSERT_TRUE(vec3f_equal(inside, spheref_closest_point(SPHERE_UNIT, inside)));
    TEST_ASSERT_TRUE(vec3f_equal(SPHERE_BOUNDARY, spheref_closest_point(SPHERE_UNIT, SPHERE_BOUNDARY)));
    TEST_ASSERT_TRUE(vec3f_equal(SPHERE_BOUNDARY, spheref_closest_point(SPHERE_UNIT, SPHERE_FAR)));
    const vec3f_t closest = spheref_closest_point(translated, outside);
    TEST_ASSERT_FLOAT_WITHIN(SPHERE_TOLERANCE, boundary.x, closest.x);
    TEST_ASSERT_FLOAT_WITHIN(SPHERE_TOLERANCE, boundary.y, closest.y);
    TEST_ASSERT_FLOAT_WITHIN(SPHERE_TOLERANCE, boundary.z, closest.z);
    TEST_ASSERT_TRUE(
        vec3f_equal(SPHERE_BOUNDARY, spheref_closest_point((spheref_t){SPHERE_BOUNDARY, mathf_zero()}, SPHERE_FAR)));
}

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

static void
test_sphere_box_corner_distance(void) {
    static const spheref_t miss = {{2, 2, 2}, 1};
    static const spheref_t touch = {{2, 3, 3}, 3};
    TEST_ASSERT_FALSE(spheref_overlaps_box(miss, SPHERE_BOX));
    TEST_ASSERT_TRUE(spheref_overlaps_box(touch, SPHERE_BOX));
}

static void
test_sphere_segment_closed_surface_and_short_segment(void) {
    static const vec3f_t short_end = {2, 0, 0};
    float t = -mathf_one();
    TEST_ASSERT_FALSE(spheref_segment_hit(SPHERE_UNIT, SPHERE_FAR, short_end, &t));
    TEST_ASSERT_TRUE(spheref_segment_hit(SPHERE_UNIT, SPHERE_BOUNDARY, SPHERE_FAR, &t));
    TEST_ASSERT_EQUAL_FLOAT(mathf_zero(), t);
    t = -mathf_one();
    TEST_ASSERT_TRUE(spheref_segment_hit(SPHERE_UNIT, SPHERE_BOUNDARY, SPHERE_UNIT.centre, &t));
    TEST_ASSERT_EQUAL_FLOAT(mathf_zero(), t);
}

static void
test_sphere_from_box_unequal_extents(void) {
    static const box3f_t box = {{-1, -2, -3}, {3, 4, 9}};
    static const spheref_t want = {{1, 1, 3}, 7};
    const spheref_t got = spheref_from_box(box);
    TEST_ASSERT_TRUE(vec3f_equal(want.centre, got.centre));
    TEST_ASSERT_FLOAT_WITHIN(SPHERE_TOLERANCE, want.radius, got.radius);
}

static void
test_sphere_long_segment_misses(void) {
    static const float half_length = 10000.0F;
    float t = -mathf_one();
    const float outside = mathf_two() * SPHERE_UNIT.radius;
    TEST_ASSERT_FALSE(
        spheref_segment_hit(SPHERE_UNIT, (vec3f_t){-half_length, outside, 0}, (vec3f_t){half_length, outside, 0}, &t));
    TEST_ASSERT_EQUAL_FLOAT(-mathf_one(), t);
}

static void
test_sphere_long_segment_entry(void) {
    static const float half_length = 10000.0F;
    static const float offset = 0.6F;
    static const float half_chord = 0.8F;
    static const float entry_tolerance = 0.0000001F;
    float t = -mathf_one();
    const float entry = (half_length - half_chord) / (mathf_two() * half_length);
    TEST_ASSERT_TRUE(
        spheref_segment_hit(SPHERE_UNIT, (vec3f_t){-half_length, offset, 0}, (vec3f_t){half_length, offset, 0}, &t));
    TEST_ASSERT_FLOAT_WITHIN(entry_tolerance, entry, t);
}

void
suite_sphere(void) {
    RUN_TEST(test_sphere_closest_point);
    RUN_TEST(test_sphere_long_segment_misses);
    RUN_TEST(test_sphere_long_segment_entry);
    RUN_TEST(test_sphere_box_corner_distance);
    RUN_TEST(test_sphere_segment_closed_surface_and_short_segment);
    RUN_TEST(test_sphere_from_box_unequal_extents);
    RUN_TEST(test_sphere_closed_queries);
    RUN_TEST(test_sphere_segment_roots_and_tangency);
}

SUITE_REGISTER(suite_sphere);
