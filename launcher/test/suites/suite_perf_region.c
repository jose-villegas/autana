/*
 * Portable suite: perf_region's named-region registry.
 */

#include <stdio.h>

#include "suites.h"
#include "unity.h"

#include "util/perf_region.h"

static void
test_a_region_is_found_by_name_even_when_its_text_has_another_address(void) {
    perf_region_registry_t registry = {0};
    perf_region_entry_t step = {.name = "step"};
    char spelled_again[] = "step";

    TEST_ASSERT_TRUE(perf_region_register(&registry, &step));
    TEST_ASSERT_EQUAL_PTR(&step, perf_region_find(&registry, spelled_again));
}

static void
test_more_regions_than_the_table_holds_are_refused(void) {
    perf_region_registry_t registry = {0};
    perf_region_entry_t entries[PERF_REGION_SLOTS + 1] = {0};
    char names[PERF_REGION_SLOTS + 1][4] = {0};

    for (int i = 0; i < PERF_REGION_SLOTS + 1; i++) {
        snprintf(names[i], sizeof names[i], "r%d", i);
        entries[i].name = names[i];
        TEST_ASSERT_EQUAL(i < PERF_REGION_SLOTS, perf_region_register(&registry, &entries[i]));
    }
    TEST_ASSERT_EQUAL_INT(PERF_REGION_SLOTS, registry.count);
    TEST_ASSERT_EQUAL_INT(1, registry.overflowed);
}

void
suite_perf_region(void) {
    RUN_TEST(test_a_region_is_found_by_name_even_when_its_text_has_another_address);
    RUN_TEST(test_more_regions_than_the_table_holds_are_refused);
}

SUITE_REGISTER(suite_perf_region);
