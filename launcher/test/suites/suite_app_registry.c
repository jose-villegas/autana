/*
 * Portable suite: app_registry.c's own contract - sorted insertion and a
 * duplicate name caught loudly rather than silently accepted.
 *
 * The duplicate-name case calls assert(), which aborts the whole process -
 * not something this suite can observe happening in its own process. It
 * re-execs this same test binary with host_main.c's hidden
 * --app-registry-duplicate-probe flag, which registers two apps under one
 * name and returns 0 only if the assert failed to catch it; the parent
 * reads that clean exit as the failure. system() has nothing to re-exec on
 * the device, so that half is host-only (#ifndef ESP_PLATFORM) - the sort
 * order half still runs there, same as everywhere else.
 */

#include <stdio.h>
#include <stdlib.h>

#include "suites.h"
#include "unity.h"

#include "app.h"

#ifndef ESP_PLATFORM
/* Set by host_main.c before any suite runs. */
extern const char* g_host_main_path;
#endif

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
    TEST_ASSERT_EQUAL_INT(3, app_registry_count());
}

#ifndef ESP_PLATFORM
static void
test_duplicate_name_aborts_instead_of_registering_silently(void) {
    char cmd[512];
    snprintf(cmd, sizeof cmd, "\"%s\" --app-registry-duplicate-probe", g_host_main_path);
    const int status = system(cmd);
    TEST_ASSERT_TRUE_MESSAGE(status != 0, "two apps sharing a name must abort registration, not both register");
}
#endif

void
run_app_registry_suite(void) {
    RUN_TEST(test_apps_end_up_sorted_by_name_regardless_of_registration_order);
#ifndef ESP_PLATFORM
    RUN_TEST(test_duplicate_name_aborts_instead_of_registering_silently);
#endif
}

SUITE_REGISTER(run_app_registry_suite);
