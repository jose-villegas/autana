#include "suites.h"

#ifndef DEVICE_BUILD

#include <string.h>

#include "unity.h"

#include "core/registry.h"

typedef struct registry_entry {
    const char* name;
    int order;
    struct registry_entry* next;
} registry_entry_t;

static registry_entry_t
fixture(const char* name, int order) {
    return (registry_entry_t){.name = name, .order = order};
}

static int
entry_order(const void* a, const void* b) {
    const registry_entry_t* first = a;
    const registry_entry_t* second = b;
    return (first->order > second->order) - (first->order < second->order);
}

static bool
entry_clash(const void* a, const void* b) {
    const registry_entry_t* first = a;
    const registry_entry_t* second = b;
    return strcmp(first->name, second->name) == 0;
}

static const registry_rule_t rule = {offsetof(registry_entry_t, next), entry_order, entry_clash};

static void
test_registration_order_does_not_change_list_order(void) {
    static const int permutations[][3] = {{0, 1, 2}, {0, 2, 1}, {1, 0, 2}, {1, 2, 0}, {2, 0, 1}, {2, 1, 0}};
    for (size_t i = 0; i < sizeof permutations / sizeof permutations[0]; i++) {
        registry_entry_t entries[] = {fixture("a", 0), fixture("b", 1), fixture("c", 2)};
        registry_entry_t* head = NULL;
        for (size_t j = 0; j < sizeof entries / sizeof entries[0]; j++) {
            TEST_ASSERT_EQUAL(REGISTRY_ADDED, registry_add(&head, &rule, &entries[permutations[i][j]]));
        }
        TEST_ASSERT_EQUAL_PTR(&entries[0], head);
        TEST_ASSERT_EQUAL_PTR(&entries[1], entries[0].next);
        TEST_ASSERT_EQUAL_PTR(&entries[2], entries[1].next);
        TEST_ASSERT_NULL(entries[2].next);
    }
}

static void
test_same_object_is_already_and_links_stay_untouched(void) {
    registry_entry_t first = fixture("a", 0), last = fixture("b", 1);
    registry_entry_t* head = &first;
    first.next = &last;
    TEST_ASSERT_TRUE(registry_contains(head, rule.link, &last));
    TEST_ASSERT_FALSE(registry_contains(NULL, rule.link, &last));
    TEST_ASSERT_EQUAL(REGISTRY_ALREADY, registry_add(&head, &rule, &last));
    TEST_ASSERT_EQUAL_PTR(&first, head);
    TEST_ASSERT_EQUAL_PTR(&last, first.next);
    TEST_ASSERT_NULL(last.next);
}

static void
test_name_clash_with_different_order_leaves_all_links_untouched(void) {
    registry_entry_t first = fixture("a", 0), last = fixture("b", 1), duplicate = fixture("b", -1);
    registry_entry_t* head = &first;
    first.next = &last;
    duplicate.next = &first;
    TEST_ASSERT_EQUAL(REGISTRY_CLASH, registry_add(&head, &rule, &duplicate));
    TEST_ASSERT_EQUAL_PTR(&first, head);
    TEST_ASSERT_EQUAL_PTR(&last, first.next);
    TEST_ASSERT_NULL(last.next);
    TEST_ASSERT_EQUAL_PTR(&first, duplicate.next);
    TEST_ASSERT_FALSE(registry_contains(head, rule.link, &duplicate));
}

static void
test_equal_order_clashes_without_a_clash_callback(void) {
    registry_entry_t first = fixture("a", 0), duplicate = fixture("b", 0);
    registry_entry_t* head = &first;
    const registry_rule_t order_rule = {offsetof(registry_entry_t, next), entry_order, NULL};
    TEST_ASSERT_EQUAL(REGISTRY_CLASH, registry_add(&head, &order_rule, &duplicate));
    TEST_ASSERT_EQUAL_PTR(&first, head);
    TEST_ASSERT_NULL(first.next);
    TEST_ASSERT_NULL(duplicate.next);
}

void
run_registry_suite(void) {
    RUN_TEST(test_registration_order_does_not_change_list_order);
    RUN_TEST(test_same_object_is_already_and_links_stay_untouched);
    RUN_TEST(test_name_clash_with_different_order_leaves_all_links_untouched);
    RUN_TEST(test_equal_order_clashes_without_a_clash_callback);
}

#else

void
run_registry_suite(void) {}

#endif

SUITE_REGISTER(run_registry_suite);
