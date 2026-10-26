#include "math/geometry/circlef.h"
#include "math/geometry/circlei.h"
#include "testkit/suite.h"
#include "unity.h"

#include <math.h>
#include <stddef.h>

static const float CIRCLE_TOLERANCE = 0.0001F;

enum { DISC_BRUTE_LIMIT = 64 };

#define CIRCLE_F(v) ((float)(v))
#define CIRCLE_I(v) (v)

#define DEFINE_CLOSED_TEST(P, V, R, C, EDGE)                                                                           \
    static void test_##P##_closed_queries(void) {                                                                      \
        const P##_t c = {{C(0), C(0)}, C(1)}, point = {{C(0), C(0)}, C(0)};                                            \
        const V##_t boundary = {C(1), C(0)}, far = {C(3), C(0)};                                                       \
        TEST_ASSERT_TRUE(P##_contains_point(c, boundary));                                                             \
        TEST_ASSERT_FALSE(P##_contains_point(c, far));                                                                 \
        TEST_ASSERT_TRUE(P##_contains_point(point, point.centre));                                                     \
        TEST_ASSERT_FALSE(P##_contains_point(point, boundary));                                                        \
        TEST_ASSERT_TRUE(P##_overlaps(point, point));                                                                  \
        TEST_ASSERT_FALSE(P##_overlaps(point, (P##_t){boundary, C(0)}));                                               \
        TEST_ASSERT_TRUE(P##_overlaps(c, (P##_t){{C(2), C(0)}, C(1)}));                                                \
        TEST_ASSERT_FALSE(P##_overlaps(c, (P##_t){far, C(1)}));                                                        \
        const R##_t rect = {C(0), C(0), C(2), C(2)};                                                                   \
        TEST_ASSERT_TRUE(P##_overlaps_rect(point, rect));                                                              \
        TEST_ASSERT_TRUE(P##_overlaps_rect((P##_t){{C(3) - EDGE, C(0)}, C(1)}, rect));                                 \
        TEST_ASSERT_FALSE(P##_overlaps_rect((P##_t){{C(4) - EDGE, C(0)}, C(1)}, rect));                                \
        TEST_ASSERT_TRUE(P##_overlaps_rect((P##_t){{C(0), C(3) - EDGE}, C(1)}, rect));                                 \
        TEST_ASSERT_FALSE(P##_overlaps_rect((P##_t){{C(0), C(4) - EDGE}, C(1)}, rect));                                \
        const R##_t wide = {C(2), C(-3), C(8), C(2)}, tall = {C(2), C(-3), C(2), C(8)};                                \
        const P##_t below = {{C(3), C(6) - EDGE}, C(1)}, right = {{C(11) - EDGE, C(-2)}, C(1)};                        \
        TEST_ASSERT_FALSE(P##_overlaps_rect(below, wide));                                                             \
        TEST_ASSERT_TRUE(P##_overlaps_rect(below, tall));                                                              \
        TEST_ASSERT_TRUE(P##_overlaps_rect(right, wide));                                                              \
        TEST_ASSERT_FALSE(P##_overlaps_rect(right, tall));                                                             \
        const R##_t bounds = P##_bounds((P##_t){{C(3), C(-2)}, C(1)});                                                 \
        TEST_ASSERT_EQUAL(C(2), bounds.x);                                                                             \
        TEST_ASSERT_EQUAL(C(-3), bounds.y);                                                                            \
        TEST_ASSERT_EQUAL(C(2) + EDGE, bounds.w);                                                                      \
        TEST_ASSERT_EQUAL(C(2) + EDGE, bounds.h);                                                                      \
    }

DEFINE_CLOSED_TEST(circlef, vec2f, rectf, CIRCLE_F, CIRCLE_F(0))
DEFINE_CLOSED_TEST(circlei, vec2i, recti, CIRCLE_I, CIRCLE_I(1))

#define DEFINE_REAL_TEST(P, V, R, T, C, TO_FLOAT)                                                                      \
    static void test_##P##_real_queries(void) {                                                                        \
        const R##_t rect = {C(-3), C(-4), C(6), C(8)};                                                                 \
        const P##_t c = P##_from_rect(rect);                                                                           \
        TEST_ASSERT_EQUAL(C(0), c.centre.x);                                                                           \
        TEST_ASSERT_EQUAL(C(0), c.centre.y);                                                                           \
        TEST_ASSERT_EQUAL(C(5), c.radius);                                                                             \
        const V##_t corners[] = {{C(-3), C(-4)}, {C(3), C(-4)}, {C(-3), C(4)}, {C(3), C(4)}};                          \
        for (size_t k = 0; k < sizeof corners / sizeof corners[0]; k++) {                                              \
            TEST_ASSERT_TRUE(P##_contains_point(c, corners[k]));                                                       \
            TEST_ASSERT_EQUAL(C(25), V##_dot(corners[k], corners[k]));                                                 \
        }                                                                                                              \
        const V##_t inside = {C(1), C(1)}, outside = {C(6), C(8)};                                                     \
        TEST_ASSERT_TRUE(V##_equal(inside, P##_closest_point(c, inside)));                                             \
        const V##_t closest = P##_closest_point(c, outside);                                                           \
        TEST_ASSERT_FLOAT_WITHIN(CIRCLE_TOLERANCE, CIRCLE_F(3), TO_FLOAT(closest.x));                                  \
        TEST_ASSERT_FLOAT_WITHIN(CIRCLE_TOLERANCE, CIRCLE_F(4), TO_FLOAT(closest.y));                                  \
        const P##_t point = {{C(0), C(0)}, C(0)};                                                                      \
        TEST_ASSERT_TRUE(V##_equal(point.centre, P##_closest_point(point, outside)));                                  \
        T t = C(-1);                                                                                                   \
        TEST_ASSERT_TRUE(P##_segment_hit(c, inside, outside, &t));                                                     \
        TEST_ASSERT_EQUAL(C(0), t);                                                                                    \
        TEST_ASSERT_TRUE(P##_segment_hit(c, inside, inside, &t));                                                      \
        TEST_ASSERT_FALSE(P##_segment_hit(c, outside, outside, &t));                                                   \
        TEST_ASSERT_TRUE(P##_segment_hit(c, (V##_t){C(-10), C(5)}, (V##_t){C(10), C(5)}, &t));                         \
        TEST_ASSERT_FLOAT_WITHIN(CIRCLE_TOLERANCE, CIRCLE_F(0.5), TO_FLOAT(t));                                        \
        TEST_ASSERT_FALSE(P##_segment_hit(c, (V##_t){C(-10), C(6)}, (V##_t){C(10), C(6)}, &t));                        \
        TEST_ASSERT_FALSE(P##_segment_hit(c, (V##_t){C(-10), C(0)}, (V##_t){C(-6), C(0)}, &t));                        \
        TEST_ASSERT_TRUE(P##_segment_hit(c, (V##_t){C(-10), C(3)}, (V##_t){C(10), C(3)}, &t));                         \
        const float half_length = CIRCLE_F(10), offset = CIRCLE_F(3), radius = CIRCLE_F(5);                            \
        const float entry = (half_length - sqrtf(radius * radius - offset * offset)) / (CIRCLE_F(2) * half_length);    \
        TEST_ASSERT_FLOAT_WITHIN(CIRCLE_TOLERANCE, entry, TO_FLOAT(t));                                                \
        TEST_ASSERT_TRUE(P##_segment_hit(c, (V##_t){C(-10), C(0)}, (V##_t){C(-5), C(0)}, &t));                         \
        TEST_ASSERT_EQUAL(C(1), t);                                                                                    \
        const V##_t entries[] = {{C(10), C(0)}, {C(0), C(-10)}, {C(6), C(8)}};                                         \
        for (size_t k = 0; k < sizeof entries / sizeof entries[0]; k++) {                                              \
            TEST_ASSERT_TRUE(P##_segment_hit(c, entries[k], V##_scale(entries[k], C(-1)), &t));                        \
            TEST_ASSERT_FLOAT_WITHIN(CIRCLE_TOLERANCE, CIRCLE_F(0.25), TO_FLOAT(t));                                   \
        }                                                                                                              \
        const V##_t boundary = {C(5), C(0)};                                                                           \
        TEST_ASSERT_TRUE(P##_segment_hit(c, boundary, outside, &t));                                                   \
        TEST_ASSERT_EQUAL(C(0), t);                                                                                    \
        TEST_ASSERT_TRUE(P##_segment_hit(c, boundary, boundary, &t));                                                  \
        TEST_ASSERT_EQUAL(C(0), t);                                                                                    \
        t = C(-1);                                                                                                     \
        TEST_ASSERT_FALSE(P##_segment_hit(c, outside, (V##_t){C(12), C(16)}, &t));                                     \
        TEST_ASSERT_EQUAL(C(-1), t);                                                                                   \
    }

DEFINE_REAL_TEST(circlef, vec2f, rectf, float, CIRCLE_F, CIRCLE_F)

static void
test_wide_disc_boundary(void) {
    enum { X = 60000, Y = 80000, RADIUS = 100000 };

    TEST_ASSERT_TRUE(mathi_in_disc(X, Y, RADIUS));
    TEST_ASSERT_TRUE(mathi_in_disc(-X, -Y, RADIUS));
    TEST_ASSERT_FALSE(mathi_in_disc(X + 1, Y, RADIUS));
    TEST_ASSERT_FALSE(mathi_in_disc(X, Y + 1, RADIUS));
    TEST_ASSERT_FALSE(mathi_in_disc(X, Y, RADIUS - 1));
}

static void
test_point_segment(void) {
    const circlef_t point = {{CIRCLE_F(2), CIRCLE_F(-3)}, CIRCLE_F(0)};
    float t = CIRCLE_F(-1);
    TEST_ASSERT_TRUE(
        circlef_segment_hit(point, (vec2f_t){CIRCLE_F(0), CIRCLE_F(-3)}, (vec2f_t){CIRCLE_F(4), CIRCLE_F(-3)}, &t));
    TEST_ASSERT_EQUAL_FLOAT(CIRCLE_F(0.5), t);
}

static void
test_cell_counts(void) {
    static const int counts[] = {1,    5,    13,   29,   49,   81,   113,  149,  197,  253,  317,
                                 377,  441,  529,  613,  709,  797,  901,  1009, 1129, 1257, 1373,
                                 1517, 1653, 1793, 1961, 2121, 2289, 2453, 2629, 2821, 3001, 3209};
    TEST_ASSERT_EQUAL_INT(0, circlei_cell_count(-1));
    for (size_t r = 0; r < sizeof counts / sizeof counts[0]; r++) {
        TEST_ASSERT_EQUAL_INT(counts[r], circlei_cell_count((int)r));
    }
    for (int r = 0; r <= DISC_BRUTE_LIMIT; r++) {
        int count = 0;
        for (int y = -r; y <= r; y++) {
            for (int x = -r; x <= r; x++) {
                const bool inside = x * x + y * y <= r * r;
                count += inside;
            }
        }
        TEST_ASSERT_EQUAL_INT(count, circlei_cell_count(r));
    }
}

void
suite_circle(void) {
    RUN_TEST(test_circlef_closed_queries);
    RUN_TEST(test_circlei_closed_queries);
    RUN_TEST(test_circlef_real_queries);
    RUN_TEST(test_wide_disc_boundary);
    RUN_TEST(test_point_segment);
    RUN_TEST(test_cell_counts);
}

SUITE_REGISTER(suite_circle);
