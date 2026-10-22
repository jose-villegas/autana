/* Command budget and touch isolation through the real settings list. */
#include "suites.h"
#include "ui_probe.h"
#include "unity.h"

#include "app/app.h"
#include "build/build_variant.h"
#include "display/display.h"
#include "gfx/gfx.h"
#include "ui/layout/ui_transform.h"
#include "ui/ui.h"

#include "shell/apps/system_settings/ui/settings_layout.h"
#include "shell/apps/system_settings/ui/settings_screen.h"

/* Landscape (448x368 logical): this project's shipping orientation. */
#define FIXTURE_QUARTER             1

/* Clock, grid and leaf are above the fold; interlace and audit need scrolling. */
#define SETTINGS_ABOVE_FOLD_TOGGLES ((1U << 3) - 1)
#define TAP_STEP_PX                 (UI_ROW_HEIGHT / 8)

static void
fixture(int quarter) {
    ui_init();
    ui_set_transform(ui_transform_quarter_turn(quarter, GFX_WIDTH, GFX_HEIGHT));
}

static void
test_settings_screen_command_list_fits_budget(void) {
    for (int quarter = DISPLAY_PORTRAIT; quarter <= DISPLAY_LANDSCAPE; quarter++) {
        fixture(quarter);
        const settings_snapshot_t state = {true, true, true, true, true};
        const input_t input = {0};
        ui_begin(&input);
        settings_screen_draw(&state);
        ui_probe_budget_assert("settings list");
    }
}

static settings_result_t
frame(const settings_snapshot_t* state, const input_t* in) {
    ui_begin(in);
    const settings_result_t result = settings_screen_draw(state);
    ui_probe_end_frame();
    return result;
}

/* Which toggles one press-and-release at (x, y) flipped, as a bit per field. */
static unsigned
tap(const settings_snapshot_t* state, int x, int y) {
    const input_t idle = {0};
    const input_t press = {.down = true, .pressed = true, .x = x, .y = y, .press_x = x, .press_y = y};
    const input_t release = {.released = true, .x = x, .y = y, .press_x = x, .press_y = y};
    (void)frame(state, &idle);
    (void)frame(state, &press);
    const settings_result_t r = frame(state, &release);
    return (unsigned)(r.values.fast_clock != state->fast_clock)
           | (unsigned)(r.values.overlay_on != state->overlay_on) << 1
           | (unsigned)(r.values.leaf_on != state->leaf_on) << 2
           | (unsigned)(r.values.interlace_on != state->interlace_on) << 3
           | (unsigned)(r.values.send_audit_on != state->send_audit_on) << 4;
}

static void
test_a_tap_on_one_toggle_changes_only_that_toggle(void) {
    for (int on = 0; on <= 1; on++) {
        fixture(FIXTURE_QUARTER);
        const settings_snapshot_t state = {on, on, on, on, on};

        /* Taps arrive in panel space, as the touch controller reports them. */
        const affine2x_t t = ui_transform_quarter_turn(FIXTURE_QUARTER, GFX_WIDTH, GFX_HEIGHT);
        unsigned reached = 0;
        unsigned row = 0;
        for (int y = 0; y < ui_height(); y += TAP_STEP_PX) {
            const vec2i_t panel = affine2x_map_pixel(&t, (vec2i_t){ui_width() / 2, y});
            const unsigned flipped = tap(&state, panel.x, panel.y);
            TEST_ASSERT_TRUE_MESSAGE((flipped & (flipped - 1)) == 0, "one tap flipped more than one toggle");
            if (flipped != 0 && flipped != row) {
                TEST_ASSERT_TRUE_MESSAGE(flipped > row, "a row flipped another row's field");
                row = flipped;
            }
            reached |= flipped;
        }

        TEST_ASSERT_EQUAL_HEX_MESSAGE(SETTINGS_ABOVE_FOLD_TOGGLES, reached, "a visible toggle was never reachable");
    }
}

void
run_settings_command_list_budget_suite(void) {
    RUN_TEST(test_settings_screen_command_list_fits_budget);
    RUN_TEST(test_a_tap_on_one_toggle_changes_only_that_toggle);
}

SUITE_REGISTER(run_settings_command_list_budget_suite);
