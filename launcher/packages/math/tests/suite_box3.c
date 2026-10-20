#include "math/geometry/box3f.h"
#include "suites.h"
#include "unity.h"

static const float BOX_TOLERANCE = 0.00001F;
static const float BOX_SCALE = 0.5F;
static const box3s_t BOX_TICKS = {{-2, -2, -2}, {2, 2, 2}};
static const vec3s_t BOX_FAR = {4, 0, 0};
static const vec3f_t BOX_ORIGIN = {0.0F, 0.0F, 0.0F};
static const vec3f_t BOX_START = {-2.0F, 0.0F, 0.0F};
static const vec3f_t BOX_END = {2.0F, 0.0F, 0.0F};
static const float BOX_ENTRY = 0.25F;
static const box3s_t BOX_FACE = {{2, -2, -2}, {4, 2, 2}};
static const box3s_t BOX_ASYMMETRIC_TICKS = {{-4, 2, 6}, {2, 6, 10}};
static const box3f_t BOX_ASYMMETRIC_UNITS = {{-2, 1, 3}, {1, 3, 5}};
static const vec3f_t BOX_ASYMMETRIC_CENTRE = {-0.5F, 2, 4};
static const vec3f_t BOX_ASYMMETRIC_HALF_EXTENT = {1.5F, 1, 1};

static void
test_box3_closed_queries_and_empty_identity(void) {
    const box3s_t empty = box3s_empty();
    const box3s_t point = box3s_from_point(BOX_FAR);
    TEST_ASSERT_TRUE(box3s_is_empty(empty));
    TEST_ASSERT_FALSE(box3s_is_empty(BOX_TICKS));
    TEST_ASSERT_TRUE(box3s_contains(box3s_extend(empty, BOX_FAR), point));
    TEST_ASSERT_TRUE(box3s_contains(box3s_union(empty, BOX_TICKS), BOX_TICKS));
    TEST_ASSERT_TRUE(box3s_contains(BOX_TICKS, box3s_union(BOX_TICKS, empty)));
    TEST_ASSERT_TRUE(box3s_contains_point(BOX_TICKS, BOX_TICKS.hi));
    TEST_ASSERT_FALSE(box3s_contains_point(BOX_TICKS, BOX_FAR));
    TEST_ASSERT_TRUE(box3s_overlaps(BOX_TICKS, BOX_FACE));
    TEST_ASSERT_TRUE(box3s_contains(BOX_TICKS, box3s_intersect(BOX_TICKS, BOX_FACE)));
    TEST_ASSERT_FALSE(box3s_contains(BOX_TICKS, BOX_FACE));
    TEST_ASSERT_TRUE(box3f_overlaps(box3f_from_box3s(BOX_TICKS, BOX_SCALE), box3f_from_box3s(BOX_FACE, BOX_SCALE)));
    const box3s_t face = box3s_from_point(BOX_TICKS.hi);
    TEST_ASSERT_TRUE(box3s_overlaps(BOX_TICKS, face));
    TEST_ASSERT_FALSE(box3s_overlaps(BOX_TICKS, point));
    TEST_ASSERT_FALSE(box3s_contains(BOX_TICKS, point));
    TEST_ASSERT_TRUE(box3s_is_empty(box3s_intersect(BOX_TICKS, point)));
    TEST_ASSERT_TRUE(box3s_contains_point(box3s_intersect(BOX_TICKS, face), BOX_TICKS.hi));
    TEST_ASSERT_TRUE(vec3s_equal(BOX_TICKS.hi, box3s_closest_point(BOX_TICKS, BOX_TICKS.hi)));
    TEST_ASSERT_TRUE(
        vec3s_equal((vec3s_t){BOX_TICKS.hi.x, BOX_FAR.y, BOX_FAR.z}, box3s_closest_point(BOX_TICKS, BOX_FAR)));
    const box3f_t b = box3f_from_box3s(BOX_TICKS, BOX_SCALE);
    TEST_ASSERT_TRUE(vec3f_equal(vec3f_scale(vec3f_from_vec3s(BOX_TICKS.hi, BOX_SCALE), -mathf_one()), b.lo));
    TEST_ASSERT_TRUE(box3f_is_empty(box3f_empty()));
    TEST_ASSERT_TRUE(box3f_contains(box3f_extend(box3f_empty(), b.lo), box3f_from_point(b.lo)));
    TEST_ASSERT_TRUE(box3f_contains(box3f_union(box3f_empty(), b), b));
    TEST_ASSERT_TRUE(box3f_contains(b, box3f_union(b, box3f_empty())));
    TEST_ASSERT_TRUE(box3f_contains_point(b, b.hi));
    TEST_ASSERT_TRUE(box3f_overlaps(b, box3f_from_point(b.hi)));
    TEST_ASSERT_FALSE(box3f_overlaps(b, box3f_from_point(BOX_END)));
    TEST_ASSERT_TRUE(box3f_is_empty(box3f_intersect(b, box3f_from_point(BOX_END))));
    TEST_ASSERT_FALSE(box3f_contains_point(b, BOX_END));
    TEST_ASSERT_FALSE(box3f_contains(b, box3f_from_point(BOX_END)));
    TEST_ASSERT_TRUE(vec3f_equal(BOX_ORIGIN, box3f_closest_point(b, BOX_ORIGIN)));
    TEST_ASSERT_TRUE(vec3f_equal((vec3f_t){b.hi.x, BOX_ORIGIN.y, BOX_ORIGIN.z}, box3f_closest_point(b, BOX_END)));
    TEST_ASSERT_TRUE(vec3f_equal(BOX_ORIGIN, box3f_centre(b)));
    TEST_ASSERT_TRUE(vec3f_equal(b.hi, box3f_half_extent(b)));
}

static void
test_box3_asymmetric_scale_and_extreme_bounds(void) {
    const box3f_t converted = box3f_from_box3s(BOX_ASYMMETRIC_TICKS, BOX_SCALE);
    TEST_ASSERT_TRUE(vec3f_equal(BOX_ASYMMETRIC_UNITS.lo, converted.lo));
    TEST_ASSERT_TRUE(vec3f_equal(BOX_ASYMMETRIC_UNITS.hi, converted.hi));
    TEST_ASSERT_TRUE(vec3f_equal(BOX_ASYMMETRIC_CENTRE, box3f_centre(converted)));
    TEST_ASSERT_TRUE(vec3f_equal(BOX_ASYMMETRIC_HALF_EXTENT, box3f_half_extent(converted)));
    const box3s_t empty = box3s_empty();
    TEST_ASSERT_TRUE(vec3s_equal((vec3s_t){INT16_MAX, INT16_MAX, INT16_MAX}, empty.lo));
    TEST_ASSERT_TRUE(vec3s_equal((vec3s_t){INT16_MIN, INT16_MIN, INT16_MIN}, empty.hi));
    const box3s_t limits = box3s_extend(box3s_from_point(empty.lo), empty.hi);
    TEST_ASSERT_TRUE(box3s_contains(limits, BOX_TICKS));
    TEST_ASSERT_TRUE(vec3s_equal(empty.hi, limits.lo));
    TEST_ASSERT_TRUE(vec3s_equal(empty.lo, limits.hi));
    const box3s_t crossed[] = {{{2, -2, -2}, {-2, 2, 2}}, {{-2, 2, -2}, {2, -2, 2}}, {{-2, -2, 2}, {2, 2, -2}}};
    for (unsigned i = 0; i < sizeof crossed / sizeof crossed[0]; i++) {
        TEST_ASSERT_TRUE(box3s_is_empty(crossed[i]));
        TEST_ASSERT_TRUE(box3f_is_empty(box3f_from_box3s(crossed[i], BOX_SCALE)));
    }
}

static void
test_box3_segment_slabs(void) {
    const box3f_t b = box3f_from_box3s(BOX_TICKS, BOX_SCALE);
    float t = -mathf_one();
    TEST_ASSERT_TRUE(box3f_segment_hit(b, BOX_START, BOX_END, &t));
    TEST_ASSERT_FLOAT_WITHIN(BOX_TOLERANCE, BOX_ENTRY, t);
    TEST_ASSERT_TRUE(box3f_segment_hit(b, BOX_END, BOX_START, &t));
    TEST_ASSERT_FLOAT_WITHIN(BOX_TOLERANCE, BOX_ENTRY, t);
    TEST_ASSERT_TRUE(box3f_segment_hit(b, BOX_ORIGIN, BOX_END, &t));
    TEST_ASSERT_EQUAL_FLOAT(mathf_zero(), t);
    TEST_ASSERT_TRUE(box3f_segment_hit(b, BOX_ORIGIN, BOX_ORIGIN, &t));
    TEST_ASSERT_FALSE(box3f_segment_hit(b, BOX_END, BOX_END, &t));
    TEST_ASSERT_FALSE(box3f_segment_hit(box3f_empty(), BOX_START, BOX_END, &t));
    const vec3f_t edge_start = {BOX_START.x, b.hi.y, b.hi.z};
    const vec3f_t edge_end = {BOX_END.x, b.hi.y, b.hi.z};
    TEST_ASSERT_TRUE(box3f_segment_hit(b, edge_start, edge_end, &t));
    TEST_ASSERT_FLOAT_WITHIN(BOX_TOLERANCE, BOX_ENTRY, t);
    TEST_ASSERT_FALSE(
        box3f_segment_hit(b, (vec3f_t){BOX_START.x, BOX_END.x, b.hi.z}, (vec3f_t){BOX_END.x, BOX_END.x, b.hi.z}, &t));
    TEST_ASSERT_TRUE(box3f_segment_hit(b, BOX_START, (vec3f_t){b.lo.x, 0, 0}, &t));
    TEST_ASSERT_EQUAL_FLOAT(mathf_one(), t);
}

static void
test_box3_planes_preserve_only_crossing_bits(void) {
    const box3f_t b = box3f_from_box3s(BOX_TICKS, BOX_SCALE);
    const planef_t planes[] = {{{1, 0, 0}, 1}, {{0, 1, 0}, 0}, {{0, 0, 1}, -2}};

    enum { INSIDE_BIT = 1u, CROSSING_BIT = 2u, OUTSIDE_BIT = 4u, PLANE_COUNT = 3 };

    unsigned mask = INSIDE_BIT | CROSSING_BIT;
    TEST_ASSERT_FALSE(box3f_outside_planes(b, planes, PLANE_COUNT, &mask));
    TEST_ASSERT_EQUAL_UINT(CROSSING_BIT, mask);
    mask = INSIDE_BIT;
    TEST_ASSERT_FALSE(box3f_outside_planes(b, planes, PLANE_COUNT, &mask));
    TEST_ASSERT_EQUAL_UINT(0, mask);
    mask = OUTSIDE_BIT;
    TEST_ASSERT_TRUE(box3f_outside_planes(b, planes, PLANE_COUNT, &mask));
    TEST_ASSERT_EQUAL_UINT(OUTSIDE_BIT, mask);
}

void
suite_box3(void) {
    RUN_TEST(test_box3_closed_queries_and_empty_identity);
    RUN_TEST(test_box3_asymmetric_scale_and_extreme_bounds);
    RUN_TEST(test_box3_segment_slabs);
    RUN_TEST(test_box3_planes_preserve_only_crossing_bits);
}

SUITE_REGISTER(suite_box3);
