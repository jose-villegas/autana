/*
 * Portable suite: measures the peak bytes the developer-toggles page's
 * command list reaches, and taps the page, driving the REAL
 * toggles_screen_draw() through ui_begin()/ui_end() as app_diagnostics.c
 * does rather than a hand-mirrored reconstruction; see
 * docs/Building-a-Screen.md.
 *
 * CONFIG_LAUNCHER_SELFTEST is never defined on a host build, so this run
 * never reaches the self-test button/result row toggles_screen.c guards on
 * it; see that file's own top comment. A SELFTEST device build's own copy
 * of this suite measures the fuller page, button and all.
 */

#include "suites.h"
#include "ui_budget.h"
#include "unity.h"

#include "app/app.h"
#include "build/build_variant.h"
#include "gfx/gfx.h"
#include "ui/layout/ui_transform.h"
#include "ui/ui.h"

#include "shell/apps/diagnostics/ui/toggles_screen.h"

/* Landscape (448x368 logical): this project's shipping orientation. */
#define FIXTURE_QUARTER    1

/* Finer than any row, so every toggle on the canvas is tapped at least once. */
#define TAP_STEP_PX        (UI_ROW_HEIGHT / 8)

/* tap()'s bits for the four rows above the fold at FIXTURE_QUARTER: overlay,
 * leaf, interlace and panel clock. */
#define ABOVE_FOLD_TOGGLES 0xFU

static void
fixture(void) {
    ui_init();
    ui_set_transform(ui_transform_quarter_turn(FIXTURE_QUARTER, GFX_WIDTH, GFX_HEIGHT));
}

static void
test_toggles_screen_command_list_fits_budget(void) {
    fixture();

    /* Worst case: every toggle on and the orientation readout showing a
     * real sample, all six of its rows drawn. */
    const toggles_screen_state_t state = {
        .overlay_on = true,
        .leaf_on = true,
        .interlace_on = true,
        .fast_clock = true,
        .send_audit_on = true,
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
    toggles_screen_draw(&state);
    ui_budget_assert("diagnostics developer toggles");
}

static toggles_screen_result_t
frame(const toggles_screen_state_t* state, const input_t* in) {
    ui_begin(in);
    const toggles_screen_result_t result = toggles_screen_draw(state);
    ui_end(TOGGLES_SCREEN_BACKGROUND);
    return result;
}

/* Which toggles one press-and-release at (x, y) flipped, as a bit per field. */
static unsigned
tap(const toggles_screen_state_t* state, int x, int y) {
    const input_t idle = {0};
    const input_t press = {.down = true, .pressed = true, .x = x, .y = y, .press_x = x, .press_y = y};
    const input_t release = {.released = true, .x = x, .y = y, .press_x = x, .press_y = y};
    (void)frame(state, &idle);
    (void)frame(state, &press);
    const toggles_screen_result_t r = frame(state, &release);
    return (unsigned)(r.overlay_on != state->overlay_on) | (unsigned)(r.leaf_on != state->leaf_on) << 1
           | (unsigned)(r.interlace_on != state->interlace_on) << 2 | (unsigned)(r.fast_clock != state->fast_clock) << 3
           | (unsigned)(r.send_audit_on != state->send_audit_on) << 4
           | (unsigned)(r.show_orientation != state->show_orientation) << 5;
}

/* Every row has its own identity and its own field: a tap anywhere flips at
 * most one toggle, the rows bind top to bottom in tap()'s bit order (the
 * order the page draws them), and each toggle above the fold is reachable.
 * The last two rows sit below the fold at this quarter and are not tapped. */
static void
test_a_tap_on_one_toggle_changes_only_that_toggle(void) {
    fixture();
    const toggles_screen_state_t state = {.overlay_on = true, .interlace_on = true};

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

    TEST_ASSERT_EQUAL_HEX_MESSAGE(ABOVE_FOLD_TOGGLES, reached & ABOVE_FOLD_TOGGLES,
                                  "a visible toggle was never reachable");
}

void
run_diagnostics_command_list_budget_suite(void) {
    RUN_TEST(test_toggles_screen_command_list_fits_budget);
    RUN_TEST(test_a_tap_on_one_toggle_changes_only_that_toggle);
}

SUITE_REGISTER(run_diagnostics_command_list_budget_suite);
