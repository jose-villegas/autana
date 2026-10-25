/*
 * Portable suite: half-open rectangles across the math number types. Right
 * and bottom edges are exclusive; empty queries have nonnegative sizes,
 * and an empty rectangle holds no point and adds nothing to a union.
 */

#include <stdbool.h>
#include <stddef.h>

#include "testkit/suite.h"
#include "unity.h"

#include "math/geometry/rectf.h"
#include "math/geometry/recti.h"
#include "math/geometry/rects.h"
#include "math/geometry/rectx.h"

enum {
    RECT_ZERO = 0,
    RECT_STEP = 1,
    RECT_LEFT = -3,
    RECT_TOP = 5,
    RECT_WIDTH = 10,
    RECT_HEIGHT = 4,
    RECT_OVERLAP_OFFSET = 2,
    RECT_SEPARATION = 2,
    RECT_DISTANCE = 5,
    RECT_NEGATIVE_WIDTH = -10,
    RECT_NEGATIVE_HEIGHT = -6,
    RECT_NEGATIVE_LEFT = 7,
    RECT_NEGATIVE_TOP = -2,
    RECT_COVER_MIN = -128,
    RECT_COVER_SIZE = 256,
};

#define RECT_TEST_F(v) ((float)(v))
#define RECT_TEST_I(v) (v)
#define RECT_TEST_S(v) ((int16_t)(v))
#define RECT_TEST_X(v) ((v) * MATHX_ONE)

#define DEFINE_RECT_TESTS(P, C, A)                                                                                     \
    static const P##_t P##_R = {C(RECT_LEFT), C(RECT_TOP), C(RECT_WIDTH), C(RECT_HEIGHT)};                             \
    static const P##_t P##_NEGATIVE_W = {C(RECT_LEFT), C(RECT_TOP), C(RECT_NEGATIVE_WIDTH), C(RECT_HEIGHT)};           \
    static const P##_t P##_NEGATIVE_H = {C(RECT_NEGATIVE_LEFT), C(RECT_NEGATIVE_TOP), C(RECT_HEIGHT),                  \
                                         C(RECT_NEGATIVE_HEIGHT)};                                                     \
    static const P##_t P##_EVERYWHERE = {C(RECT_COVER_MIN), C(RECT_COVER_MIN), C(RECT_COVER_SIZE),                     \
                                         C(RECT_COVER_SIZE)};                                                          \
                                                                                                                       \
    static void P##_assert_rect(P##_t want, P##_t got) {                                                               \
        A(want.x, got.x);                                                                                              \
        A(want.y, got.y);                                                                                              \
        A(want.w, got.w);                                                                                              \
        A(want.h, got.h);                                                                                              \
    }                                                                                                                  \
                                                                                                                       \
    static void test_##P##_right_and_bottom_are_exclusive(void) {                                                      \
        A(P##_R.x + P##_R.w, P##_right(P##_R));                                                                        \
        A(P##_R.y + P##_R.h, P##_bottom(P##_R));                                                                       \
        TEST_ASSERT_TRUE(P##_contains(P##_R, P##_right(P##_R) - C(RECT_STEP), P##_bottom(P##_R) - C(RECT_STEP)));      \
        TEST_ASSERT_FALSE(P##_contains(P##_R, P##_right(P##_R), P##_bottom(P##_R) - C(RECT_STEP)));                    \
        TEST_ASSERT_FALSE(P##_contains(P##_R, P##_right(P##_R) - C(RECT_STEP), P##_bottom(P##_R)));                    \
    }                                                                                                                  \
                                                                                                                       \
    static void test_##P##_from_edges_round_trips_and_crossed_edges_are_empty(void) {                                  \
        P##_assert_rect(P##_R, P##_from_edges(P##_R.x, P##_R.y, P##_right(P##_R), P##_bottom(P##_R)));                 \
        const P##_t crossed = P##_from_edges(P##_R.x, P##_R.y, P##_R.x - C(RECT_STEP), P##_R.y - C(RECT_STEP));        \
        A(C(RECT_ZERO), crossed.w);                                                                                    \
        A(C(RECT_ZERO), crossed.h);                                                                                    \
        TEST_ASSERT_TRUE(P##_is_empty(crossed));                                                                       \
        TEST_ASSERT_TRUE(P##_is_empty(P##_from_edges(P##_R.x, P##_R.y, P##_R.x, P##_bottom(P##_R))));                  \
        TEST_ASSERT_FALSE(P##_is_empty(P##_R));                                                                        \
    }                                                                                                                  \
                                                                                                                       \
    static void test_##P##_contains_holds_the_left_and_top_edges_only(void) {                                          \
        TEST_ASSERT_TRUE(P##_contains(P##_R, P##_R.x, P##_R.y));                                                       \
        TEST_ASSERT_FALSE(P##_contains(P##_R, P##_R.x - C(RECT_STEP), P##_R.y));                                       \
        TEST_ASSERT_FALSE(P##_contains(P##_R, P##_R.x, P##_R.y - C(RECT_STEP)));                                       \
        TEST_ASSERT_FALSE(P##_contains((P##_t){P##_R.x, P##_R.y, C(RECT_ZERO), P##_R.h}, P##_R.x, P##_R.y));           \
    }                                                                                                                  \
                                                                                                                       \
    static void test_##P##_intersect_keeps_the_overlap(void) {                                                         \
        const P##_t other = {P##_R.x + C(RECT_OVERLAP_OFFSET), P##_R.y - C(RECT_STEP), P##_R.w, P##_R.h};              \
        const P##_t want = P##_from_edges(other.x, P##_R.y, P##_right(P##_R), P##_bottom(other));                      \
        P##_assert_rect(want, P##_intersect(P##_R, other));                                                            \
        P##_assert_rect(want, P##_intersect(other, P##_R));                                                            \
        P##_assert_rect(P##_R, P##_intersect(P##_R, P##_R));                                                           \
    }                                                                                                                  \
                                                                                                                       \
    static void test_##P##_intersect_of_disjoint_or_touching_rects_is_empty(void) {                                    \
        const P##_t beside = {P##_right(P##_R), P##_R.y, P##_R.w, P##_R.h};                                            \
        const P##_t below = {P##_R.x, P##_bottom(P##_R), P##_R.w, P##_R.h};                                            \
        const P##_t far = {P##_right(P##_R) + P##_R.w, P##_bottom(P##_R) + P##_R.h, P##_R.w, P##_R.h};                 \
        const P##_t got[] = {P##_intersect(P##_R, beside), P##_intersect(P##_R, below), P##_intersect(P##_R, far)};    \
        for (size_t i = 0; i < sizeof got / sizeof got[0]; i++) {                                                      \
            TEST_ASSERT_TRUE(P##_is_empty(got[i]));                                                                    \
            TEST_ASSERT_TRUE(got[i].w >= C(RECT_ZERO) && got[i].h >= C(RECT_ZERO));                                    \
        }                                                                                                              \
    }                                                                                                                  \
                                                                                                                       \
    static void test_##P##_union_covers_both_and_skips_an_empty_one(void) {                                            \
        const P##_t other = {P##_R.x + P##_R.w * RECT_SEPARATION, P##_R.y - P##_R.h, C(RECT_STEP), C(RECT_STEP)};      \
        const P##_t want = P##_from_edges(P##_R.x, other.y, P##_right(other), P##_bottom(P##_R));                      \
        P##_assert_rect(want, P##_union(P##_R, other));                                                                \
        P##_assert_rect(want, P##_union(other, P##_R));                                                                \
                                                                                                                       \
        /* An empty rect far away must not stretch the union to reach it. */                                           \
        const P##_t empty_far = {P##_R.x - P##_R.w * RECT_DISTANCE, P##_R.y + P##_R.h * RECT_DISTANCE, C(RECT_ZERO),   \
                                 C(RECT_ZERO)};                                                                        \
        P##_assert_rect(P##_R, P##_union(P##_R, empty_far));                                                           \
        P##_assert_rect(P##_R, P##_union(empty_far, P##_R));                                                           \
    }                                                                                                                  \
                                                                                                                       \
    static void test_##P##_negative_sizes_are_empty_and_hold_nothing(void) {                                           \
        const P##_t negatives[] = {P##_NEGATIVE_W, P##_NEGATIVE_H};                                                    \
        for (size_t i = 0; i < sizeof negatives / sizeof negatives[0]; i++) {                                          \
            const P##_t n = negatives[i];                                                                              \
            TEST_ASSERT_TRUE(P##_is_empty(n));                                                                         \
            TEST_ASSERT_FALSE(P##_contains(n, n.x, n.y));                                                              \
            TEST_ASSERT_FALSE(P##_contains(n, n.x + n.w, n.y + n.h));                                                  \
            const P##_t got = P##_intersect(n, P##_EVERYWHERE);                                                        \
            TEST_ASSERT_TRUE(P##_is_empty(got));                                                                       \
            TEST_ASSERT_TRUE(got.w >= C(RECT_ZERO) && got.h >= C(RECT_ZERO));                                          \
        }                                                                                                              \
    }                                                                                                                  \
                                                                                                                       \
    static void test_##P##_union_of_two_empty_rects_is_empty_with_no_negative_size(void) {                             \
        const P##_t pairs[][2] = {{P##_NEGATIVE_W, P##_NEGATIVE_H}, {P##_NEGATIVE_H, P##_NEGATIVE_W}};                 \
        for (size_t i = 0; i < sizeof pairs / sizeof pairs[0]; i++) {                                                  \
            const P##_t got = P##_union(pairs[i][0], pairs[i][1]);                                                     \
            TEST_ASSERT_TRUE(P##_is_empty(got));                                                                       \
            A(C(RECT_ZERO), got.w);                                                                                    \
            A(C(RECT_ZERO), got.h);                                                                                    \
        }                                                                                                              \
    }

DEFINE_RECT_TESTS(rectf, RECT_TEST_F, TEST_ASSERT_EQUAL_FLOAT)
DEFINE_RECT_TESTS(recti, RECT_TEST_I, TEST_ASSERT_EQUAL_INT)
DEFINE_RECT_TESTS(rects, RECT_TEST_S, TEST_ASSERT_EQUAL_INT16)
DEFINE_RECT_TESTS(rectx, RECT_TEST_X, TEST_ASSERT_EQUAL_INT32)

void
suite_recti(void) {
    RUN_TEST(test_rectf_right_and_bottom_are_exclusive);
    RUN_TEST(test_rectf_from_edges_round_trips_and_crossed_edges_are_empty);
    RUN_TEST(test_rectf_contains_holds_the_left_and_top_edges_only);
    RUN_TEST(test_rectf_intersect_keeps_the_overlap);
    RUN_TEST(test_rectf_intersect_of_disjoint_or_touching_rects_is_empty);
    RUN_TEST(test_rectf_union_covers_both_and_skips_an_empty_one);
    RUN_TEST(test_rectf_negative_sizes_are_empty_and_hold_nothing);
    RUN_TEST(test_rectf_union_of_two_empty_rects_is_empty_with_no_negative_size);
    RUN_TEST(test_recti_right_and_bottom_are_exclusive);
    RUN_TEST(test_recti_from_edges_round_trips_and_crossed_edges_are_empty);
    RUN_TEST(test_recti_contains_holds_the_left_and_top_edges_only);
    RUN_TEST(test_recti_intersect_keeps_the_overlap);
    RUN_TEST(test_recti_intersect_of_disjoint_or_touching_rects_is_empty);
    RUN_TEST(test_recti_union_covers_both_and_skips_an_empty_one);
    RUN_TEST(test_recti_negative_sizes_are_empty_and_hold_nothing);
    RUN_TEST(test_recti_union_of_two_empty_rects_is_empty_with_no_negative_size);
    RUN_TEST(test_rects_right_and_bottom_are_exclusive);
    RUN_TEST(test_rects_from_edges_round_trips_and_crossed_edges_are_empty);
    RUN_TEST(test_rects_contains_holds_the_left_and_top_edges_only);
    RUN_TEST(test_rects_intersect_keeps_the_overlap);
    RUN_TEST(test_rects_intersect_of_disjoint_or_touching_rects_is_empty);
    RUN_TEST(test_rects_union_covers_both_and_skips_an_empty_one);
    RUN_TEST(test_rects_negative_sizes_are_empty_and_hold_nothing);
    RUN_TEST(test_rects_union_of_two_empty_rects_is_empty_with_no_negative_size);
    RUN_TEST(test_rectx_right_and_bottom_are_exclusive);
    RUN_TEST(test_rectx_from_edges_round_trips_and_crossed_edges_are_empty);
    RUN_TEST(test_rectx_contains_holds_the_left_and_top_edges_only);
    RUN_TEST(test_rectx_intersect_keeps_the_overlap);
    RUN_TEST(test_rectx_intersect_of_disjoint_or_touching_rects_is_empty);
    RUN_TEST(test_rectx_union_covers_both_and_skips_an_empty_one);
    RUN_TEST(test_rectx_negative_sizes_are_empty_and_hold_nothing);
    RUN_TEST(test_rectx_union_of_two_empty_rects_is_empty_with_no_negative_size);
}

SUITE_REGISTER(suite_recti);
