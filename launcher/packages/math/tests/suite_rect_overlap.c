/*
 * Portable suite: overlaps found in a list of recti by one sort and sweep,
 * along either axis, and a rectangle with another taken out of it.
 */

#include <stdbool.h>
#include <string.h>

#include "testkit/suite.h"
#include "unity.h"

#include "math/geometry/rect_overlap.h"

enum {
    LIST_MAX = 8,
    FIELD = 24,
};

/* Whether any two of `r` share a pixel, checked pair by pair. */
static bool
any_pair_overlaps(const recti_t* r, int count) {
    for (int i = 0; i < count; i++) {
        for (int j = i + 1; j < count; j++) {
            if (!recti_is_empty(recti_intersect(r[i], r[j]))) {
                return true;
            }
        }
    }
    return false;
}

/* The sweep finds an overlapping pair exactly when one exists, along either
 * axis, and the pair it names overlaps. */
static void
assert_sweep_agrees(const recti_t* in, int count) {
    for (int axis = 0; axis < 2; axis++) {
        recti_t r[LIST_MAX];
        memcpy(r, in, (size_t)count * sizeof r[0]);
        int j = -1;
        const int i = recti_overlapping_pair(r, count, axis == 0, &j);
        TEST_ASSERT_EQUAL(any_pair_overlaps(in, count), i >= 0);
        if (i >= 0) {
            TEST_ASSERT_TRUE(j > i && j < count);
            TEST_ASSERT_FALSE(recti_is_empty(recti_intersect(r[i], r[j])));
        }
    }
}

static void
test_the_sweep_finds_a_pair_exactly_when_two_overlap(void) {
    /* Touching rects share no pixel. */
    assert_sweep_agrees((const recti_t[]){{0, 0, 4, 4}, {4, 0, 4, 4}, {0, 4, 4, 4}}, 3);
    /* A wide first rect reaches one that starts far after it. */
    assert_sweep_agrees((const recti_t[]){{0, 0, 20, 2}, {2, 6, 2, 2}, {16, 1, 2, 2}}, 3);
    /* A tall rect reaches one far below its start. */
    assert_sweep_agrees((const recti_t[]){{1, 0, 2, 20}, {6, 2, 2, 2}, {2, 16, 2, 2}}, 3);
    assert_sweep_agrees((const recti_t[]){{5, 5, 1, 1}}, 1);
    assert_sweep_agrees((const recti_t[]){{0, 0, 1, 1}}, 0);
}

/* Every pair of placements on a small field, both orders. */
static void
test_the_sweep_agrees_with_a_pair_check_on_every_placement(void) {
    const recti_t still = {8, 8, 6, 4};
    for (int y = 0; y < FIELD; y += 2) {
        for (int x = 0; x < FIELD; x += 2) {
            const recti_t moving = {x, y, 5, 3};
            assert_sweep_agrees((const recti_t[]){still, moving, {0, 20, 3, 3}}, 3);
            assert_sweep_agrees((const recti_t[]){moving, {0, 20, 3, 3}, still}, 3);
        }
    }
}

/* The pieces are disjoint, miss `b`, and with a ∩ b cover `a` exactly. */
static void
assert_subtracts(recti_t a, recti_t b, int want_most) {
    recti_t out[RECTI_SUBTRACT_MAX];
    const int n = recti_subtract(a, b, out);
    TEST_ASSERT_TRUE(n <= want_most);
    TEST_ASSERT_FALSE(any_pair_overlaps(out, n));
    int area = recti_area(recti_intersect(a, b));
    for (int i = 0; i < n; i++) {
        TEST_ASSERT_TRUE(recti_is_empty(recti_intersect(out[i], b)));
        TEST_ASSERT_TRUE(recti_area(recti_intersect(out[i], a)) == recti_area(out[i]));
        area += recti_area(out[i]);
    }
    TEST_ASSERT_EQUAL_INT(recti_area(a), area);
}

static void
test_subtract_leaves_disjoint_pieces_covering_the_rest(void) {
    const recti_t a = {4, 4, 10, 8};
    assert_subtracts(a, (recti_t){6, 6, 2, 2}, RECTI_SUBTRACT_MAX);
    assert_subtracts(a, (recti_t){0, 6, 30, 2}, 2);
    assert_subtracts(a, (recti_t){10, 0, 10, 30}, 1);
    assert_subtracts(a, (recti_t){12, 10, 10, 10}, RECTI_SUBTRACT_MAX - 1);
    assert_subtracts(a, (recti_t){30, 30, 2, 2}, 1);
    assert_subtracts(a, (recti_t){0, 0, 40, 40}, 0);
    for (int y = 0; y < FIELD; y += 3) {
        for (int x = 0; x < FIELD; x += 3) {
            assert_subtracts(a, (recti_t){x, y, 5, 4}, RECTI_SUBTRACT_MAX);
        }
    }
}

void
suite_rect_overlap(void) {
    RUN_TEST(test_the_sweep_finds_a_pair_exactly_when_two_overlap);
    RUN_TEST(test_the_sweep_agrees_with_a_pair_check_on_every_placement);
    RUN_TEST(test_subtract_leaves_disjoint_pieces_covering_the_rest);
}

SUITE_REGISTER(suite_rect_overlap);
