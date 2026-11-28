#include "ball_cases.h"
#include "math/geometry/spheref.h"
#include "math/geometry/spheres.h"
#include "testkit/suite.h"
#include "unity.h"

#include <stddef.h>
#include <stdint.h>

DEFINE_BALL_CONTAINMENT_TEST(spheref, vec3f, float)
DEFINE_BALL_CONTAINMENT_TEST(spheres, vec3s, int16_t)

static const float SPHERE_TOLERANCE = 0.00001F;
static const spheref_t SPHERE_UNIT = {{0, 0, 0}, 1};
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
    const vec3f_t inside = vec3f_scale(vec3f_unit_x(), mathf_half());
    TEST_ASSERT_TRUE(vec3f_equal(inside, spheref_closest_point(SPHERE_UNIT, inside)));
    TEST_ASSERT_TRUE(vec3f_equal(vec3f_unit_x(), spheref_closest_point(SPHERE_UNIT, vec3f_unit_x())));
    TEST_ASSERT_TRUE(vec3f_equal(vec3f_unit_x(), spheref_closest_point(SPHERE_UNIT, SPHERE_FAR)));
    const vec3f_t closest = spheref_closest_point(translated, outside);
    TEST_ASSERT_FLOAT_WITHIN(SPHERE_TOLERANCE, boundary.x, closest.x);
    TEST_ASSERT_FLOAT_WITHIN(SPHERE_TOLERANCE, boundary.y, closest.y);
    TEST_ASSERT_FLOAT_WITHIN(SPHERE_TOLERANCE, boundary.z, closest.z);
    TEST_ASSERT_TRUE(
        vec3f_equal(vec3f_unit_x(), spheref_closest_point((spheref_t){vec3f_unit_x(), mathf_zero()}, SPHERE_FAR)));
}

static void
test_sphere_closed_queries(void) {
    const spheref_t enclosing = spheref_from_box(SPHERE_FLAT_BOX);
    TEST_ASSERT_TRUE(vec3f_equal(SPHERE_FLAT_ENCLOSING.centre, enclosing.centre));
    TEST_ASSERT_FLOAT_WITHIN(SPHERE_TOLERANCE, SPHERE_FLAT_ENCLOSING.radius, enclosing.radius);
    TEST_ASSERT_TRUE(spheref_contains_point(SPHERE_UNIT, SPHERE_UNIT.centre));
    TEST_ASSERT_TRUE(spheref_contains_point(SPHERE_UNIT, vec3f_unit_x()));
    TEST_ASSERT_FALSE(spheref_contains_point(SPHERE_UNIT, SPHERE_FAR));
    TEST_ASSERT_TRUE(spheref_overlaps(SPHERE_UNIT, (spheref_t){vec3f_unit_x(), SPHERE_UNIT.radius}));
    TEST_ASSERT_TRUE(
        spheref_overlaps(SPHERE_UNIT, (spheref_t){vec3f_scale(vec3f_unit_x(), mathf_two()), SPHERE_UNIT.radius}));
    TEST_ASSERT_FALSE(spheref_overlaps(SPHERE_UNIT, (spheref_t){SPHERE_FAR, SPHERE_UNIT.radius}));
    TEST_ASSERT_TRUE(spheref_overlaps_box(SPHERE_UNIT, SPHERE_BOX));
    TEST_ASSERT_TRUE(
        spheref_overlaps_box((spheref_t){vec3f_scale(vec3f_unit_x(), mathf_two()), SPHERE_UNIT.radius}, SPHERE_BOX));
    TEST_ASSERT_FALSE(spheref_overlaps_box((spheref_t){SPHERE_FAR, SPHERE_UNIT.radius}, SPHERE_BOX));
    TEST_ASSERT_TRUE(spheref_contains_point((spheref_t){vec3f_unit_x(), 0}, vec3f_unit_x()));
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
    TEST_ASSERT_TRUE(spheref_segment_hit(SPHERE_UNIT, SPHERE_FAR, vec3f_unit_x(), &t));
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
    TEST_ASSERT_TRUE(spheref_segment_hit(SPHERE_UNIT, vec3f_unit_x(), SPHERE_FAR, &t));
    TEST_ASSERT_EQUAL_FLOAT(mathf_zero(), t);
    t = -mathf_one();
    TEST_ASSERT_TRUE(spheref_segment_hit(SPHERE_UNIT, vec3f_unit_x(), SPHERE_UNIT.centre, &t));
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

static void
test_spheres_ticks(void) {
    static const spheres_t ticks = {{8, -16, 24}, 4};
    static const float units_per_tick = 0.125F;
    static const spheref_t units = {{1, -2, 3}, 0.5F};
    TEST_ASSERT_TRUE(spheres_contains_point(ticks, (vec3s_t){12, -16, 24}));
    TEST_ASSERT_FALSE(spheres_contains_point(ticks, (vec3s_t){13, -16, 24}));
    TEST_ASSERT_TRUE(spheres_overlaps(ticks, (spheres_t){{16, -16, 24}, 4}));
    TEST_ASSERT_FALSE(spheres_overlaps(ticks, (spheres_t){{17, -16, 24}, 4}));
    const spheref_t converted = spheref_from_spheres(ticks, units_per_tick);
    TEST_ASSERT_TRUE(vec3f_equal(units.centre, converted.centre));
    TEST_ASSERT_EQUAL_FLOAT(units.radius, converted.radius);
    /* far + reach ticks apart: more than an int16 holds, where a saturated
     * difference would land just inside; the test widens. */
    static const int16_t far = 30000, reach = 3000;
    const spheres_t east = {{far, 0, 0}, INT16_MAX};
    TEST_ASSERT_FALSE(spheres_contains_ball(east, (spheres_t){{(int16_t)-reach, 0, 0}, 0}, 0));
    const spheres_t touching = {{(int16_t)(far - INT16_MAX + 1), 0, 0}, 1};
    TEST_ASSERT_TRUE(spheres_contains_ball(east, touching, 0));
    TEST_ASSERT_FALSE(spheres_contains_ball(east, touching, 1));
}

static void
test_spheres_contains_ball_extremes(void) {
    /* Zero room requires coincidence, and one tick of room includes touching. */
    const int16_t extra_tick_margin = -1;
    const spheres_t point = {{INT16_MAX, 0, 0}, 0};
    const spheres_t one_tick_away = {{INT16_MAX - 1, 0, 0}, 0};
    const spheres_t two_ticks_away = {{INT16_MAX - 2, 0, 0}, 0};
    TEST_ASSERT_TRUE(spheres_contains_ball(point, one_tick_away, extra_tick_margin));
    TEST_ASSERT_FALSE(spheres_contains_ball(point, two_ticks_away, extra_tick_margin));
    TEST_ASSERT_TRUE(spheres_contains_ball(point, point, 0));
    TEST_ASSERT_FALSE(spheres_contains_ball(point, one_tick_away, 0));

    /* Opposite corners require wide differences and squared distance. */
    TEST_ASSERT_FALSE(spheres_contains_ball((spheres_t){{INT16_MIN, INT16_MIN, INT16_MIN}, INT16_MAX},
                                            (spheres_t){{INT16_MAX, INT16_MAX, INT16_MAX}, 0}, 0));

    /* Touching and one tick over agree in both directions. */
    const spheres_t west = {{INT16_MIN, 0, 0}, INT16_MAX};
    const spheres_t east = {{INT16_MAX, 0, 0}, INT16_MAX};
    const spheres_t midpoint = {{0, 0, 0}, 0};
    const spheres_t west_of_midpoint = {{-1, 0, 0}, 0};
    TEST_ASSERT_TRUE(spheres_contains_ball(east, midpoint, 0));
    TEST_ASSERT_FALSE(spheres_contains_ball(east, west_of_midpoint, 0));
    TEST_ASSERT_TRUE(spheres_contains_ball(west, west_of_midpoint, 0));
    TEST_ASSERT_FALSE(spheres_contains_ball(west, midpoint, 0));
    const int16_t clearance_margin = 1;
    const spheres_t east_of_midpoint = {{1, 0, 0}, 0};
    TEST_ASSERT_TRUE(spheres_contains_ball(east, east_of_midpoint, clearance_margin));
    TEST_ASSERT_FALSE(spheres_contains_ball(east, midpoint, clearance_margin));

    /* Saturating both squares hides the offset beyond the full x span. */
    TEST_ASSERT_FALSE(spheres_contains_ball(west, (spheres_t){{INT16_MAX, 1, 0}, 0}, INT16_MIN));

    /* The minimum margin admits a gap one tick above INT16_MAX. */
    const spheres_t wide_touch = {{0, 0, 0}, INT16_MAX};
    const spheres_t wide_miss = {{1, 0, 0}, INT16_MAX};
    TEST_ASSERT_TRUE(spheres_contains_ball(west, wide_touch, INT16_MIN));
    TEST_ASSERT_FALSE(spheres_contains_ball(west, wide_miss, INT16_MIN));

    /* A z-only gap straddles room one tick short of the full span. */
    const spheres_t low = {{0, 0, INT16_MIN}, INT16_MAX};
    const spheres_t depth_inside = {{0, 0, INT16_MAX - 2}, 0};
    const spheres_t depth_outside = {{0, 0, INT16_MAX}, 0};
    const int16_t depth_margin = INT16_MIN + 1;
    TEST_ASSERT_TRUE(spheres_contains_ball(low, depth_inside, depth_margin));
    TEST_ASSERT_FALSE(spheres_contains_ball(low, depth_outside, depth_margin));

    /* The most negative room excludes even concentric balls. */
    TEST_ASSERT_FALSE(spheres_contains_ball((spheres_t){{0, 0, 0}, 0}, (spheres_t){{0, 0, 0}, INT16_MAX}, INT16_MAX));
}

static void
test_sphere_contains_ball_depth(void) {
    /* The z-only offset can fall inside, touch or exceed the room. */
    const spheref_t outer = {{2, -3, 4}, 10};
    const spheref_t touching = {{2, -3, 9}, 5};
    const spheref_t inside = {{touching.centre.x, touching.centre.y, touching.centre.z - 1}, touching.radius};
    const spheref_t outside = {{2, -3, 10}, 5};
    TEST_ASSERT_TRUE(spheref_contains_ball(outer, inside, 0));
    TEST_ASSERT_TRUE(spheref_contains_ball(outer, touching, 0));
    TEST_ASSERT_FALSE(spheref_contains_ball(outer, outside, 0));
    TEST_ASSERT_TRUE(spheref_contains_ball(outer, outside, -1));
}

void
suite_sphere(void) {
    RUN_TEST(test_spheres_ticks);
    RUN_TEST(test_spheref_contains_ball);
    RUN_TEST(test_spheres_contains_ball);
    RUN_TEST(test_spheres_contains_ball_extremes);
    RUN_TEST(test_sphere_contains_ball_depth);
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
