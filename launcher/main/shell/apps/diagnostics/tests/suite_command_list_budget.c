/* Command budget with the full orientation readout and optional self-test controls. */
#include "suites.h"
#include "ui_probe.h"
#include "unity.h"

#include "app/app.h"
#include "build/build_variant.h"
#include "gfx/gfx.h"
#include "ui/layout/ui_transform.h"
#include "ui/ui.h"

#include "shell/apps/diagnostics/ui/tools_screen.h"

/* Landscape (448x368 logical): this project's shipping orientation. */
#define FIXTURE_QUARTER 1

static void
fixture(void) {
    ui_init();
    ui_set_transform(ui_transform_quarter_turn(FIXTURE_QUARTER, GFX_WIDTH, GFX_HEIGHT));
}

static void
test_diagnostics_tools_screen_command_list_fits_budget(void) {
    fixture();

    /* A full-scale sample draws every orientation row. */
    const diagnostics_tools_screen_state_t state = {
        .show_orientation = true,
        .imu_ready = true,
        .have_sample = true,
        .accel_ax = -32768,
        .accel_ay = -32768,
        .accel_az = -32768,
        .gravity_gx = -32768,
        .gravity_gy = -32768,
        .shell_quarter = 3,
    };

    const input_t input = {0};
    ui_begin(&input);
    diagnostics_tools_screen_draw(&state);
    ui_probe_budget_assert("orientation and self test");
}

void
run_diagnostics_command_budget_suite(void) {
    RUN_TEST(test_diagnostics_tools_screen_command_list_fits_budget);
}

SUITE_REGISTER(run_diagnostics_command_budget_suite);
