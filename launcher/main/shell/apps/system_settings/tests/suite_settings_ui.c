/* Each edited value requests only its own owner setter, in either direction. */
#include "shell/apps/system_settings/settings_ui.h"
#include "suites.h"
#include "unity.h"

static void
assert_changes(settings_changes_t changes, int selected) {
    TEST_ASSERT_EQUAL(selected == 0, changes.panel_clock);
    TEST_ASSERT_EQUAL(selected == 1, changes.overlay);
    TEST_ASSERT_EQUAL(selected == 2, changes.leaf);
    TEST_ASSERT_EQUAL(selected == 3, changes.interlace);
    TEST_ASSERT_EQUAL(selected == 4, changes.send_audit);
}

static void
test_unchanged_snapshot_requests_no_setter(void) {
    for (int on = 0; on <= 1; on++) {
        const settings_snapshot_t snapshot = {on, on, on, on, on};
        const settings_result_t result = {.values = snapshot};
        assert_changes(settings_ui_changes(&snapshot, &result), -1);
    }
}

static void
test_each_edit_requests_only_its_owner_setter(void) {
    for (int on = 0; on <= 1; on++) {
        const settings_snapshot_t snapshot = {on, on, on, on, on};
        settings_result_t result = {.values = snapshot};
        bool* fields[] = {&result.values.fast_clock, &result.values.overlay_on, &result.values.leaf_on,
                          &result.values.interlace_on, &result.values.send_audit_on};
        for (size_t i = 0; i < sizeof fields / sizeof fields[0]; i++) {
            *fields[i] = !on;
            assert_changes(settings_ui_changes(&snapshot, &result), (int)i);
            *fields[i] = on;
        }
    }
}

void
run_settings_ui_suite(void) {
    RUN_TEST(test_unchanged_snapshot_requests_no_setter);
    RUN_TEST(test_each_edit_requests_only_its_owner_setter);
}

SUITE_REGISTER(run_settings_ui_suite);
