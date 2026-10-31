#include "math/geometry/box3f.h"
#include "math/geometry/box3f_plane.h"
#include "math/geometry/planex.h"
#include "testkit/suite.h"
#include "unity.h"

MATH_DEFINE_BOX3(box3x, vec3x, int32_t, mathx, INT32_MIN, INT32_MAX)
MATH_DEFINE_BOX3_REAL(box3x, vec3x, planex, int32_t, mathx)

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

#define EXPECT_BOX3_EQUAL(P, V, want, got)                                                                             \
    do {                                                                                                               \
        const P##_t expected_box = (want);                                                                             \
        const P##_t actual_box = (got);                                                                                \
        TEST_ASSERT_TRUE(V##_equal(expected_box.lo, actual_box.lo) && V##_equal(expected_box.hi, actual_box.hi));      \
    } while (0)

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
    TEST_ASSERT_TRUE(vec3f_equal(b.lo, box3f_corner(b, 0)));
    TEST_ASSERT_TRUE(vec3f_equal(b.hi, box3f_corner(b, 7)));
    TEST_ASSERT_TRUE(vec3f_equal((vec3f_t){b.hi.x, b.lo.y, b.hi.z}, box3f_corner(b, 5)));
    TEST_ASSERT_TRUE(
        vec3s_equal((vec3s_t){BOX_TICKS.lo.x, BOX_TICKS.hi.y, BOX_TICKS.lo.z}, box3s_corner(BOX_TICKS, 2)));
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

static void
test_box3_segment_direction_and_finite_interval(void) {
    static const struct {
        vec3f_t a, c;
        bool hit;
    } segments[] = {
        {{-3, 0, 0}, {-2, 0, 0}, false}, {{2, 0, 0}, {3, 0, 0}, false}, {{-3, 0, 0}, {3, 6, 0}, false},
        {{0, -2, 0}, {0, 2, 0}, true},   {{0, 0, -2}, {0, 0, 2}, true}, {{-2, 2, -2}, {2, -2, 2}, true},
    };

    const box3f_t b = box3f_from_box3s(BOX_TICKS, BOX_SCALE);
    for (unsigned i = 0; i < sizeof segments / sizeof segments[0]; i++) {
        float t = -mathf_one();
        TEST_ASSERT_EQUAL(segments[i].hit, box3f_segment_hit(b, segments[i].a, segments[i].c, &t));
        if (segments[i].hit) {
            TEST_ASSERT_FLOAT_WITHIN(BOX_TOLERANCE, BOX_ENTRY, t);
        }
    }
}

static void
test_box3_plane_oblique_touch_and_late_rejection(void) {
    const box3f_t b = box3f_from_box3s(BOX_TICKS, BOX_SCALE);
    static const planef_t planes[] = {{{-1, -2, -3}, -5}, {{1, 0, 0}, -1}, {{0, -1, 0}, 1}, {{0, 0, -1}, -2}};

    enum { OBLIQUE = 1u, OUTER_TOUCH = 2u, INNER_TOUCH = 4u, REJECT = 8u, COUNT = 4 };

    unsigned mask = OBLIQUE | OUTER_TOUCH | INNER_TOUCH;
    TEST_ASSERT_FALSE(box3f_outside_planes(b, planes, COUNT, &mask));
    TEST_ASSERT_EQUAL_UINT(OBLIQUE | OUTER_TOUCH, mask);
    mask = INNER_TOUCH | REJECT;
    TEST_ASSERT_TRUE(box3f_outside_planes(b, planes, COUNT, &mask));
    TEST_ASSERT_EQUAL_UINT(REJECT, mask);
    mask = REJECT;
    TEST_ASSERT_FALSE(box3f_outside_planes(b, planes, 0, &mask));
    TEST_ASSERT_EQUAL_UINT(REJECT, mask);
}

enum { BOX_CORNERS = 8 };

/* The least and greatest signed distance of b's corners: the box's own, as
 * a plane is linear and a box is the hull of its corners. */
static void
corner_extremes(box3f_t b, planef_t plane, float* least, float* most) {
    *least = INFINITY;
    *most = -INFINITY;
    for (int k = 0; k < BOX_CORNERS; k++) {
        const float d = planef_distance(plane, box3f_corner(b, k));
        *least = mathf_min(*least, d);
        *most = mathf_max(*most, d);
    }
}

static void
test_box3_plane_distance_axis_aligned_is_exact(void) {
    const box3f_t b = box3f_from_box3s(BOX_ASYMMETRIC_TICKS, BOX_SCALE);
    const float offset = BOX_ASYMMETRIC_CENTRE.y;
    /* x - offset, and its flip offset - x: the faces give the answer. */
    const planef_t up = {{1.0F, 0.0F, 0.0F}, -offset};
    const planef_t down = {{-1.0F, 0.0F, 0.0F}, offset};
    TEST_ASSERT_EQUAL_FLOAT(b.lo.x - offset, box3f_plane_distance_min(b, up));
    TEST_ASSERT_EQUAL_FLOAT(b.hi.x - offset, box3f_plane_distance_max(b, up));
    TEST_ASSERT_EQUAL_FLOAT(offset - b.hi.x, box3f_plane_distance_min(b, down));
    TEST_ASSERT_EQUAL_FLOAT(offset - b.lo.x, box3f_plane_distance_max(b, down));
}

static void
test_box3_plane_distance_oblique_matches_the_corners(void) {
    const box3f_t b = box3f_from_box3s(BOX_ASYMMETRIC_TICKS, BOX_SCALE);
    static const planef_t planes[] = {
        {{1.0F, 2.0F, -3.0F}, 0.5F}, {{-1.0F, -2.0F, 3.0F}, -0.5F}, {{-0.25F, 0.0F, -4.0F}, 7.0F}};
    for (unsigned i = 0; i < sizeof planes / sizeof planes[0]; i++) {
        float least, most;
        corner_extremes(b, planes[i], &least, &most);
        TEST_ASSERT_FLOAT_WITHIN(BOX_TOLERANCE, least, box3f_plane_distance_min(b, planes[i]));
        TEST_ASSERT_FLOAT_WITHIN(BOX_TOLERANCE, most, box3f_plane_distance_max(b, planes[i]));
    }
}

/* The least z on a box where a constraint holds. */
static void
test_box3_plane_distance_min_inside_one_half_space(void) {
    const box3f_t cube = box3f_from_box3s(BOX_TICKS, BOX_SCALE);
    const planef_t depth = {{0.0F, 0.0F, 1.0F}, 0.0F};
    /* z >= x holds at the corner (lo.x, y, lo.z): it never binds. */
    const planef_t loose = {{-1.0F, 0.0F, 1.0F}, 0.0F};
    TEST_ASSERT_FLOAT_WITHIN(BOX_TOLERANCE, cube.lo.z, box3f_plane_distance_min_inside(cube, depth, loose));
    /* z >= x + (hi.z - lo.z) / 2 binds at x = lo.x. */
    const float rise = (cube.hi.z - cube.lo.z) * 0.5F;
    const planef_t binding = {{-1.0F, 0.0F, 1.0F}, -rise};
    TEST_ASSERT_FLOAT_WITHIN(BOX_TOLERANCE, cube.lo.x + rise, box3f_plane_distance_min_inside(cube, depth, binding));
    /* A thin slab seen from above, as a ground square: z >= reach - slope y
     * keeps only z >= reach - slope hi.y, at the top face, while the box
     * alone reaches lo.z. */
    const box3f_t slab = {{cube.lo.x, cube.hi.y - BOX_SCALE, cube.lo.z}, cube.hi};
    const float slope = 2.0F;
    const float reach = (slope * slab.hi.y) + (cube.hi.z * 0.5F); /* binds halfway up the cube */
    const planef_t bottom = {{0.0F, slope, 1.0F}, -reach};
    TEST_ASSERT_EQUAL_FLOAT(slab.lo.z, box3f_plane_distance_min(slab, depth));
    TEST_ASSERT_FLOAT_WITHIN(BOX_TOLERANCE, reach - (slope * slab.hi.y),
                             box3f_plane_distance_min_inside(slab, depth, bottom));
    /* A box outside the half-space has no least value; the bound never falls
     * under the box's own. */
    const planef_t beyond = {{1.0F, 0.0F, 0.0F}, -(cube.hi.x + 1.0F)};
    TEST_ASSERT_TRUE(box3f_plane_distance_min_inside(cube, depth, beyond) >= box3f_plane_distance_min(cube, depth));
}

static void
test_box3_set_operations_each_axis(void) {
    static const box3s_t other = {{-4, 0, -1}, {0, 4, 1}};
    static const box3s_t intersection = {{-2, 0, -1}, {0, 2, 1}};
    static const box3s_t joined = {{-4, -2, -2}, {2, 4, 2}};
    static const vec3s_t grow = {-4, 4, 0};
    static const vec3s_t outside[] = {{-4, 0, 0}, {4, 0, 0}, {0, -4, 0}, {0, 4, 0}, {0, 0, -4}, {0, 0, 4}, {-4, 4, -4}};
    static const vec3s_t closest[] = {{-2, 0, 0}, {2, 0, 0}, {0, -2, 0}, {0, 2, 0}, {0, 0, -2}, {0, 0, 2}, {-2, 2, -2}};
    const box3s_t si = box3s_intersect(BOX_TICKS, other);
    const box3s_t su = box3s_union(BOX_TICKS, other);
    const box3s_t se = box3s_extend(BOX_TICKS, grow);
    EXPECT_BOX3_EQUAL(box3s, vec3s, intersection, si);
    EXPECT_BOX3_EQUAL(box3s, vec3s, joined, su);
    EXPECT_BOX3_EQUAL(box3s, vec3s, joined, se);
    const box3f_t b = box3f_from_box3s(BOX_TICKS, BOX_SCALE);
    const box3f_t fi = box3f_intersect(b, box3f_from_box3s(other, BOX_SCALE));
    const box3f_t fu = box3f_union(b, box3f_from_box3s(other, BOX_SCALE));
    const box3f_t fe = box3f_extend(b, vec3f_from_vec3s(grow, BOX_SCALE));
    const box3f_t want_i = box3f_from_box3s(intersection, BOX_SCALE);
    const box3f_t want_u = box3f_from_box3s(joined, BOX_SCALE);
    EXPECT_BOX3_EQUAL(box3f, vec3f, want_i, fi);
    EXPECT_BOX3_EQUAL(box3f, vec3f, want_u, fu);
    EXPECT_BOX3_EQUAL(box3f, vec3f, want_u, fe);
    for (unsigned i = 0; i < sizeof outside / sizeof outside[0]; i++) {
        const box3s_t point = box3s_from_point(outside[i]);
        TEST_ASSERT_FALSE(box3s_contains_point(BOX_TICKS, outside[i]));
        TEST_ASSERT_FALSE(box3s_contains(BOX_TICKS, point));
        TEST_ASSERT_FALSE(box3s_overlaps(BOX_TICKS, point));
        TEST_ASSERT_TRUE(box3s_is_empty(box3s_intersect(BOX_TICKS, point)));
        TEST_ASSERT_TRUE(vec3s_equal(closest[i], box3s_closest_point(BOX_TICKS, outside[i])));
        const vec3f_t p = vec3f_from_vec3s(outside[i], BOX_SCALE);
        const box3f_t fp = box3f_from_point(p);
        TEST_ASSERT_FALSE(box3f_contains_point(b, p));
        TEST_ASSERT_FALSE(box3f_contains(b, fp));
        TEST_ASSERT_FALSE(box3f_overlaps(b, fp));
        TEST_ASSERT_TRUE(box3f_is_empty(box3f_intersect(b, fp)));
        TEST_ASSERT_TRUE(vec3f_equal(vec3f_from_vec3s(closest[i], BOX_SCALE), box3f_closest_point(b, p)));
    }
}

static void
test_box3_real_fixed_scalar_ops(void) {
    const box3x_t b = {vec3x_from_vec3f(BOX_ASYMMETRIC_UNITS.lo), vec3x_from_vec3f(BOX_ASYMMETRIC_UNITS.hi)};
    TEST_ASSERT_TRUE(vec3x_equal(vec3x_from_vec3f(BOX_ASYMMETRIC_CENTRE), box3x_centre(b)));
    TEST_ASSERT_TRUE(vec3x_equal(vec3x_from_vec3f(BOX_ASYMMETRIC_HALF_EXTENT), box3x_half_extent(b)));
    static const planex_t plane = {{-MATHX_ONE, -MATHX_ONE, -MATHX_ONE}, 7 * MATHX_ONE};

    enum { PLANE_BIT = 1u, COUNT = 1 };

    unsigned mask = PLANE_BIT;
    TEST_ASSERT_FALSE(box3x_outside_planes(b, &plane, COUNT, &mask));
    TEST_ASSERT_EQUAL_UINT(PLANE_BIT, mask);
}

static void
test_box3_empty_operands_never_overlap(void) {
    static const box3s_t crossed[] = {
        {{1, -1, -1}, {-1, 1, 1}},
        {{-1, 1, -1}, {1, -1, 1}},
        {{-1, -1, 1}, {1, 1, -1}},
    };
    const box3f_t b = box3f_from_box3s(BOX_TICKS, BOX_SCALE);
    TEST_ASSERT_FALSE(box3s_overlaps(box3s_empty(), BOX_TICKS));
    TEST_ASSERT_FALSE(box3s_overlaps(BOX_TICKS, box3s_empty()));
    TEST_ASSERT_FALSE(box3f_overlaps(box3f_empty(), b));
    TEST_ASSERT_FALSE(box3f_overlaps(b, box3f_empty()));
    for (unsigned i = 0; i < sizeof crossed / sizeof crossed[0]; i++) {
        const box3f_t empty = box3f_from_box3s(crossed[i], BOX_SCALE);
        TEST_ASSERT_FALSE(box3s_overlaps(crossed[i], BOX_TICKS));
        TEST_ASSERT_FALSE(box3s_overlaps(BOX_TICKS, crossed[i]));
        TEST_ASSERT_FALSE(box3s_overlaps(crossed[i], crossed[i]));
        TEST_ASSERT_FALSE(box3f_overlaps(empty, b));
        TEST_ASSERT_FALSE(box3f_overlaps(b, empty));
        TEST_ASSERT_FALSE(box3f_overlaps(empty, empty));
    }
}

void
suite_box3(void) {
    RUN_TEST(test_box3_empty_operands_never_overlap);
    RUN_TEST(test_box3_real_fixed_scalar_ops);
    RUN_TEST(test_box3_segment_direction_and_finite_interval);
    RUN_TEST(test_box3_plane_oblique_touch_and_late_rejection);
    RUN_TEST(test_box3_set_operations_each_axis);
    RUN_TEST(test_box3_closed_queries_and_empty_identity);
    RUN_TEST(test_box3_asymmetric_scale_and_extreme_bounds);
    RUN_TEST(test_box3_segment_slabs);
    RUN_TEST(test_box3_planes_preserve_only_crossing_bits);
    RUN_TEST(test_box3_plane_distance_axis_aligned_is_exact);
    RUN_TEST(test_box3_plane_distance_oblique_matches_the_corners);
    RUN_TEST(test_box3_plane_distance_min_inside_one_half_space);
}

SUITE_REGISTER(suite_box3);
