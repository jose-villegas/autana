/*
 * Portable suite: math/geometry/recti.h. A rect is half-open, so its edges are
 * one past its last pixel; queries that find nothing return a size of zero,
 * never a negative one, and an empty rect neither holds a point nor adds to a
 * union.
 */

#include <limits.h>
#include <stdbool.h>
#include <stddef.h>

#include "suites.h"
#include "unity.h"

#include "math/geometry/recti.h"

static void
assert_rect(recti_t want, recti_t got) {
    TEST_ASSERT_EQUAL_INT(want.x, got.x);
    TEST_ASSERT_EQUAL_INT(want.y, got.y);
    TEST_ASSERT_EQUAL_INT(want.w, got.w);
    TEST_ASSERT_EQUAL_INT(want.h, got.h);
}

/* A rect away from the origin, with negative corners in reach of its tests. */
static const recti_t R = {-3, 5, 10, 4};

static void
test_right_and_bottom_are_one_past_the_last_pixel(void) {
    TEST_ASSERT_EQUAL_INT(R.x + R.w, recti_right(R));
    TEST_ASSERT_EQUAL_INT(R.y + R.h, recti_bottom(R));
    TEST_ASSERT_TRUE(recti_contains(R, recti_right(R) - 1, recti_bottom(R) - 1));
    TEST_ASSERT_FALSE(recti_contains(R, recti_right(R), recti_bottom(R) - 1));
    TEST_ASSERT_FALSE(recti_contains(R, recti_right(R) - 1, recti_bottom(R)));
}

static void
test_from_edges_round_trips_and_crossed_edges_are_empty(void) {
    assert_rect(R, recti_from_edges(R.x, R.y, recti_right(R), recti_bottom(R)));
    const recti_t crossed = recti_from_edges(R.x, R.y, R.x - 1, R.y - 1);
    TEST_ASSERT_EQUAL_INT(0, crossed.w);
    TEST_ASSERT_EQUAL_INT(0, crossed.h);
    TEST_ASSERT_TRUE(recti_is_empty(crossed));
    TEST_ASSERT_TRUE(recti_is_empty(recti_from_edges(R.x, R.y, R.x, recti_bottom(R))));
    TEST_ASSERT_FALSE(recti_is_empty(R));
}

static void
test_contains_holds_the_left_and_top_edges_only(void) {
    TEST_ASSERT_TRUE(recti_contains(R, R.x, R.y));
    TEST_ASSERT_FALSE(recti_contains(R, R.x - 1, R.y));
    TEST_ASSERT_FALSE(recti_contains(R, R.x, R.y - 1));
    TEST_ASSERT_FALSE(recti_contains((recti_t){R.x, R.y, 0, R.h}, R.x, R.y));
}

static void
test_intersect_keeps_the_overlap(void) {
    const recti_t other = {R.x + 2, R.y - 1, R.w, R.h};
    const recti_t want = recti_from_edges(other.x, R.y, recti_right(R), recti_bottom(other));
    assert_rect(want, recti_intersect(R, other));
    assert_rect(want, recti_intersect(other, R));
    assert_rect(R, recti_intersect(R, R));
}

static void
test_intersect_of_disjoint_or_touching_rects_is_empty(void) {
    const recti_t beside = {recti_right(R), R.y, R.w, R.h};
    const recti_t below = {R.x, recti_bottom(R), R.w, R.h};
    const recti_t far = {recti_right(R) + R.w, recti_bottom(R) + R.h, R.w, R.h};
    const recti_t got[] = {recti_intersect(R, beside), recti_intersect(R, below), recti_intersect(R, far)};
    for (size_t i = 0; i < sizeof got / sizeof got[0]; i++) {
        TEST_ASSERT_TRUE(recti_is_empty(got[i]));
        TEST_ASSERT_TRUE(got[i].w >= 0 && got[i].h >= 0);
    }
}

static void
test_union_covers_both_and_skips_an_empty_one(void) {
    const recti_t other = {R.x + R.w * 2, R.y - R.h, 1, 1};
    const recti_t want = recti_from_edges(R.x, other.y, recti_right(other), recti_bottom(R));
    assert_rect(want, recti_union(R, other));
    assert_rect(want, recti_union(other, R));

    /* An empty rect far away must not stretch the union to reach it. */
    const recti_t empty_far = {R.x - R.w * 5, R.y + R.h * 5, 0, 0};
    assert_rect(R, recti_union(R, empty_far));
    assert_rect(R, recti_union(empty_far, R));
}

/* Rects a caller built with a negative size: empty, like a zero one. */
static const recti_t NEGATIVE_W = {-3, 5, -10, 4};
static const recti_t NEGATIVE_H = {7, -2, 4, -6};
/* Holds every point the tests use, so only the other rect can empty an
 * intersection with it. */
static const recti_t EVERYWHERE = {INT_MIN / 2, INT_MIN / 2, INT_MAX, INT_MAX};

static void
test_negative_sizes_are_empty_and_hold_nothing(void) {
    const recti_t negatives[] = {NEGATIVE_W, NEGATIVE_H};
    for (size_t i = 0; i < sizeof negatives / sizeof negatives[0]; i++) {
        const recti_t n = negatives[i];
        TEST_ASSERT_TRUE(recti_is_empty(n));
        TEST_ASSERT_FALSE(recti_contains(n, n.x, n.y));
        TEST_ASSERT_FALSE(recti_contains(n, n.x + n.w, n.y + n.h));
        const recti_t got = recti_intersect(n, EVERYWHERE);
        TEST_ASSERT_TRUE(recti_is_empty(got));
        TEST_ASSERT_TRUE(got.w >= 0 && got.h >= 0);
    }
}

static void
test_union_of_two_empty_rects_is_empty_with_no_negative_size(void) {
    const recti_t pairs[][2] = {{NEGATIVE_W, NEGATIVE_H}, {NEGATIVE_H, NEGATIVE_W}};
    for (size_t i = 0; i < sizeof pairs / sizeof pairs[0]; i++) {
        const recti_t got = recti_union(pairs[i][0], pairs[i][1]);
        TEST_ASSERT_TRUE(recti_is_empty(got));
        TEST_ASSERT_EQUAL_INT(0, got.w);
        TEST_ASSERT_EQUAL_INT(0, got.h);
    }
}

void
suite_recti(void) {
    RUN_TEST(test_right_and_bottom_are_one_past_the_last_pixel);
    RUN_TEST(test_from_edges_round_trips_and_crossed_edges_are_empty);
    RUN_TEST(test_contains_holds_the_left_and_top_edges_only);
    RUN_TEST(test_intersect_keeps_the_overlap);
    RUN_TEST(test_intersect_of_disjoint_or_touching_rects_is_empty);
    RUN_TEST(test_union_covers_both_and_skips_an_empty_one);
    RUN_TEST(test_negative_sizes_are_empty_and_hold_nothing);
    RUN_TEST(test_union_of_two_empty_rects_is_empty_with_no_negative_size);
}

SUITE_REGISTER(suite_recti);
