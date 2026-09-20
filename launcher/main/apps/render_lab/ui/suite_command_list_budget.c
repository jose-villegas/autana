/*
 * Portable suite: measures the peak bytes this app's two microui screens'
 * command list reaches, driving the REAL render_lab_hud_screen_draw()/
 * render_lab_menu_screen_draw() against a real microui (ui_init() +
 * ui_begin(), the same calls app_render_lab.c makes) rather than a
 * hand-mirrored reconstruction - see docs/Building-a-Screen.md.
 */

#include "suites.h"
#include "unity.h"

#include "app.h"
#include "gfx/gfx.h"
#include "ui/ui.h"
#include "ui/ui_transform.h"

#include "render_lab_hud_screen.h"
#include "render_lab_menu_screen.h"

/* A busier icon or a third screen still has this many bytes of
 * MU_COMMANDLIST_SIZE left to grow into before either screen's own peak
 * reaches the ceiling microui.c asserts against. */
#define COMMANDLIST_HEADROOM_BYTES 2048
#define COMMANDLIST_BUDGET         (MU_COMMANDLIST_SIZE - COMMANDLIST_HEADROOM_BYTES)

/* Landscape (448x368 logical) - this app's shipping orientation. */
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
test_hud_screen_command_list_fits_budget(void) {
    fixture();

    /* Worst case: all three boxes at once, with the longest scene name and
     * the widest status a wire scene reports (wire_primitives_generated.h's
     * own 1024/2048 mesh-wide caps). */
    const render_lab_hud_screen_state_t state = {
        .fps_value = 999.9,
        .fps_box_x_override = -1,
        .scene_title = "Wire Capsule",
        .scene_title_alpha = 255,
        .status = "1024v 2048e",
    };

    const input_t input = {0};
    ui_begin(&input);
    render_lab_hud_screen_draw(ui_context(), &state);
    assert_budget("render lab HUD", end_and_measure());
}

static void
test_menu_screen_command_list_fits_budget(void) {
    fixture();

    const render_lab_menu_screen_state_t state = {
        .partial_updates_on = true,
        .band_mode_on = true,
        .scene_name = "Gouraud Cube",
    };

    const input_t input = {0};
    ui_begin(&input);
    render_lab_menu_screen_draw(ui_context(), &state, 16);
    assert_budget("render lab menu", end_and_measure());
}

void
run_render_lab_command_list_budget_suite(void) {
    RUN_TEST(test_hud_screen_command_list_fits_budget);
    RUN_TEST(test_menu_screen_command_list_fits_budget);
}

SUITE_REGISTER(run_render_lab_command_list_budget_suite);
