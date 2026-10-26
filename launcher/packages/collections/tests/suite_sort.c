#include <stdbool.h>
#include <stddef.h>
#include <stdlib.h>

#include "testkit/suite.h"
#include "unity.h"

#include "collections/sort.h"

typedef struct {
    int key;
    int arrival;
} keyed_t;

static int
compare_int(const void* a, const void* b) {
    const int x = *(const int*)a;
    const int y = *(const int*)b;
    return (x > y) - (x < y);
}

static int
by_key(const void* a, const void* b) {
    return compare_int(&((const keyed_t*)a)->key, &((const keyed_t*)b)->key);
}

typedef struct {
    uint8_t bytes[SORT_ELEMENT_MAX];
} widest_t;

static inline bool
key_before(keyed_t a, keyed_t b) {
    return a.key < b.key;
}

SORT_DEFINE_INSERTION(keyed, keyed_t, key_before)

static int
by_first_byte(const void* a, const void* b) {
    return (int)((const widest_t*)a)->bytes[0] - (int)((const widest_t*)b)->bytes[0];
}

static void
test_sort_insertion_orders_like_qsort(void) {
    enum { COUNT = 97 };

    int* sorted = malloc(sizeof(int) * COUNT);
    int* expected = malloc(sizeof(int) * COUNT);
    TEST_ASSERT_NOT_NULL(sorted);
    TEST_ASSERT_NOT_NULL(expected);
    unsigned state = 12345U;
    for (int i = 0; i < COUNT; i++) {
        state = (state * 1103515245U) + 12345U;
        sorted[i] = (int)(state >> 16) - 32768;
        expected[i] = sorted[i];
    }
    sort_insertion(sorted, COUNT, sizeof sorted[0], compare_int);
    qsort(expected, COUNT, sizeof expected[0], compare_int);
    TEST_ASSERT_EQUAL_INT_ARRAY(expected, sorted, COUNT);
    free(sorted);
    free(expected);
}

static void
test_sort_insertion_keeps_equal_keys_in_arrival_order(void) {
    keyed_t items[] = {{2, 0}, {1, 1}, {2, 2}, {0, 3}, {1, 4}, {2, 5}};
    const int arrivals[] = {3, 1, 4, 0, 2, 5};
    sort_insertion(items, sizeof items / sizeof items[0], sizeof items[0], by_key);
    for (size_t i = 0; i < sizeof items / sizeof items[0]; i++) {
        TEST_ASSERT_EQUAL_INT(arrivals[i], items[i].arrival);
    }
}

static void
test_sort_insertion_leaves_short_arrays_alone(void) {
    int one = 7;
    sort_insertion(&one, 1, sizeof one, compare_int);
    TEST_ASSERT_EQUAL_INT(7, one);
    sort_insertion(NULL, 0, sizeof one, compare_int);
}

static void
test_sort_insertion_moves_the_widest_element_whole(void) {
    widest_t items[3] = {0};
    for (int i = 0; i < 3; i++) {
        for (size_t b = 0; b < SORT_ELEMENT_MAX; b++) {
            items[i].bytes[b] = (uint8_t)(3 - i);
        }
    }
    sort_insertion(items, 3, sizeof items[0], by_first_byte);
    for (int i = 0; i < 3; i++) {
        for (size_t b = 0; b < SORT_ELEMENT_MAX; b++) {
            TEST_ASSERT_EQUAL_UINT8(i + 1, items[i].bytes[b]);
        }
    }
}

static void
test_typed_sort_keeps_equal_keys_in_arrival_order(void) {
    keyed_t items[] = {{2, 0}, {1, 1}, {2, 2}, {0, 3}, {1, 4}, {2, 5}};
    const int arrivals[] = {3, 1, 4, 0, 2, 5};
    keyed_sort(items, sizeof items / sizeof items[0]);
    for (size_t i = 0; i < sizeof items / sizeof items[0]; i++) {
        TEST_ASSERT_EQUAL_INT(arrivals[i], items[i].arrival);
    }
}

static void
test_typed_insert_builds_the_same_order_one_item_at_a_time(void) {
    const keyed_t arriving[] = {{2, 0}, {1, 1}, {2, 2}, {0, 3}, {1, 4}, {2, 5}};
    const int arrivals[] = {3, 1, 4, 0, 2, 5};
    keyed_t items[sizeof arriving / sizeof arriving[0]];
    for (size_t i = 0; i < sizeof arriving / sizeof arriving[0]; i++) {
        keyed_insert(items, i, arriving[i]);
    }
    for (size_t i = 0; i < sizeof items / sizeof items[0]; i++) {
        TEST_ASSERT_EQUAL_INT(arrivals[i], items[i].arrival);
    }
}

void
run_sort_suite(void) {
    RUN_TEST(test_typed_sort_keeps_equal_keys_in_arrival_order);
    RUN_TEST(test_typed_insert_builds_the_same_order_one_item_at_a_time);
    RUN_TEST(test_sort_insertion_orders_like_qsort);
    RUN_TEST(test_sort_insertion_keeps_equal_keys_in_arrival_order);
    RUN_TEST(test_sort_insertion_leaves_short_arrays_alone);
    RUN_TEST(test_sort_insertion_moves_the_widest_element_whole);
}

SUITE_REGISTER(run_sort_suite);
