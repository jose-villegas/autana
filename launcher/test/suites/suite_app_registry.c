/*
 * Host-only suite: app_registry.c's own sort order. A real boot's registry
 * must never be wiped mid-run the way a test process's list is between
 * scenarios.
 */

#include "suites.h"

#ifndef DEVICE_BUILD

#include "unity.h"

#include "app.h"

static app_t apps[4];

static void
fixture(void) {
    app_registry_reset_for_test();
}

static void
register_named(int slot, const char* name) {
    apps[slot] = (app_t){.name = name};
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

#else

void
run_app_registry_suite(void) {}

#endif

SUITE_REGISTER(run_app_registry_suite);
