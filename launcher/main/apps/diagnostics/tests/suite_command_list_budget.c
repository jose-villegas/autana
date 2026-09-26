/*
 * Portable suite: measures the peak bytes the developer-toggles page's
 * command list reaches, driving the REAL toggles_screen_draw() against a
 * real microui (ui_init() + ui_begin(), the same calls app_diagnostics.c
 * makes) rather than a hand-mirrored reconstruction - see
 * docs/Building-a-Screen.md.
 *
 * CONFIG_LAUNCHER_SELFTEST is never defined on a host build, so this run
 * never reaches the self-test button/result row toggles_screen.c guards on
 * it - see that file's own top comment. A SELFTEST device build's own copy
 * of this suite measures the fuller page, button and all.
 */

#include <string.h>

#include "suites.h"
#include "unity.h"

#include "app.h"
#include "build_variant.h"
#include "gfx/gfx.h"
#include "ui/ui.h"
#include "ui/ui_transform.h"

#include "apps/diagnostics/ui/toggles_screen.h"

/* The host's larger command list checks rendering growth; device/QEMU enforces
 * the 8 KiB RAM limit. A future toggle still has this many bytes of MU_COMMANDLIST_SIZE left to
 * grow into before this page's own peak reaches the ceiling microui.c
 * asserts against. */
#define COMMANDLIST_HEADROOM_BYTES 2048
#define COMMANDLIST_BUDGET         (MU_COMMANDLIST_SIZE - COMMANDLIST_HEADROOM_BYTES)

/* Landscape (448x368 logical) - this project's shipping orientation. */
static void
fixture(void) {
    ui_init();
    ui_set_transform(ui_transform_quarter_turn(1, GFX_WIDTH, GFX_HEIGHT));
}

static int
end_and_measure(void) {
    mu_end(ui_context());
    return ui_context()->command_list.idx;
}

static void
assert_budget(const char* screen_name, int used) {
    char msg[96];
    snprintf(msg, sizeof msg, "%s screen used %d of %d budget bytes (%d headroom)", screen_name, used,
             COMMANDLIST_BUDGET, MU_COMMANDLIST_SIZE - used);
    printf("%s\n", msg);
    TEST_ASSERT_LESS_THAN_INT_MESSAGE(COMMANDLIST_BUDGET, used, msg);
}

static void
test_toggles_screen_command_list_fits_budget(void) {
    fixture();

    /* Worst case: every checkbox on and the orientation readout showing a
     * real sample, all three of its rows drawn. */
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
    toggles_screen_draw(ui_context(), &state);
    assert_budget("diagnostics developer toggles", end_and_measure());
}

static bool
find_text_pos(const char* str, mu_Vec2* out) {
    mu_Command* cmd = NULL;
    while (mu_next_command(ui_context(), &cmd)) {
        if (cmd->type == MU_COMMAND_TEXT && strcmp(cmd->text.str, str) == 0) {
            *out = cmd->text.pos;
            return true;
        }
    }
    return false;
}

static void
test_a_tap_on_one_toggle_changes_only_that_toggle(void) {
    fixture();
    mu_Context* ctx = ui_context();

    const toggles_screen_state_t state = {.overlay_on = true, .interlace_on = true};

    const input_t input = {0};
    ui_begin(&input);
    toggles_screen_draw(ctx, &state);
    mu_end(ctx);

    mu_Vec2 at;
    TEST_ASSERT_TRUE_MESSAGE(find_text_pos("gfx leaf-rect overlay", &at), "the leaf row's label was never drawn");

    /* microui only focuses a control it already saw hovered, and only
     * hovers inside the window the previous frame ended over: one frame to
     * enter the window, one to hover the row, then the press. */
    mu_input_mousemove(ctx, at.x, at.y);
    for (int frame = 0; frame < 2; frame++) {
        mu_begin(ctx);
        toggles_screen_draw(ctx, &state);
        mu_end(ctx);
    }

    mu_input_mousedown(ctx, at.x, at.y, MU_MOUSE_LEFT);
    mu_begin(ctx);
    const toggles_screen_result_t result = toggles_screen_draw(ctx, &state);
    mu_end(ctx);

    TEST_ASSERT_TRUE_MESSAGE(result.leaf_on, "the tapped toggle did not flip");
    TEST_ASSERT_TRUE_MESSAGE(result.overlay_on, "the row above flipped with it");
    TEST_ASSERT_TRUE_MESSAGE(result.interlace_on, "the row below flipped with it");
    TEST_ASSERT_FALSE(result.fast_clock);
    TEST_ASSERT_FALSE(result.send_audit_on);
    TEST_ASSERT_FALSE(result.show_orientation);
}

void
run_diagnostics_command_list_budget_suite(void) {
    RUN_TEST(test_toggles_screen_command_list_fits_budget);
    RUN_TEST(test_a_tap_on_one_toggle_changes_only_that_toggle);
}

SUITE_REGISTER(run_diagnostics_command_list_budget_suite);
