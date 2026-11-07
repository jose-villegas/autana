/* Command budget of the self-test page. */
#include <limits.h>

#include "suites.h"
#include "ui_probe.h"
#include "unity.h"

#include "app/app.h"
#include "gfx/gfx.h"
#include "ui/layout/ui_transform.h"
#include "ui/ui.h"

#include "shell/apps/diagnostics/ui/selftest_screen.h"

/* Landscape (448x368 logical): this project's shipping orientation. */
#define FIXTURE_QUARTER 1

static void
fixture(void) {
    ui_init();
    ui_set_transform(ui_transform_quarter_turn(FIXTURE_QUARTER, GFX_WIDTH, GFX_HEIGHT));
}

static void
test_diagnostics_selftest_screen_command_list_fits_budget(void) {
    fixture();

    /* The widest result line. */
    const diagnostics_selftest_screen_state_t state = {.failures = INT_MAX};

    const input_t input = {0};
    ui_begin(&input);
    (void)diagnostics_selftest_screen_draw(&state);
    ui_probe_budget_assert("self test");
}

void
run_diagnostics_command_budget_suite(void) {
    RUN_TEST(test_diagnostics_selftest_screen_command_list_fits_budget);
}

SUITE_REGISTER(run_diagnostics_command_budget_suite);
