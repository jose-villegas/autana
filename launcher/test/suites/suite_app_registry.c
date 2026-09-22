/*
 * Host-only suite: app_registry.c's own sort order. Not linked into the
 * device selftest - app_registry_reset_for_test() is absent from every
 * device build, and a real boot's registry must never be wiped mid-run
 * the way a test process's own list is between scenarios.
 */

#include <stdio.h>

#include "suites.h"
#include "unity.h"

#include "app.h"

static app_t apps[4];
static char names[4][16];

static void
fixture(void) {
    app_registry_reset_for_test();
}

static void
register_named(int slot, const char* name) {
    snprintf(names[slot], sizeof names[slot], "%s", name);
    apps[slot] = (app_t){.name = names[slot]};
    app_register(&apps[slot]);
}

static void
test_apps_end_up_sorted_by_name_regardless_of_registration_order(void) {
    fixture();
    register_named(0, "Zulu");
    register_named(1, "Alpha");
    register_named(2, "Mike");

    const app_t* app = app_list();
    TEST_ASSERT_NOT_NULL(app);
    TEST_ASSERT_EQUAL_STRING("Alpha", app->name);
    app = app->next;
    TEST_ASSERT_NOT_NULL(app);
    TEST_ASSERT_EQUAL_STRING("Mike", app->name);
    app = app->next;
    TEST_ASSERT_NOT_NULL(app);
    TEST_ASSERT_EQUAL_STRING("Zulu", app->name);
    TEST_ASSERT_NULL(app->next);
}

void
run_app_registry_suite(void) {
    RUN_TEST(test_apps_end_up_sorted_by_name_regardless_of_registration_order);
}

SUITE_REGISTER(run_app_registry_suite);
