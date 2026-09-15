/*
 * Portable suite: measures the peak bytes this app's two microui screens'
 * command list reaches, driving the REAL cube_hud_screen_draw()/
 * cube_menu_screen_draw() against a real microui (ui_init() + ui_begin(),
 * the same calls app_cube.c makes) rather than a hand-mirrored
 * reconstruction - see docs/Building-a-Screen.md.
 */

#include "suites.h"
#include "unity.h"

#include "app.h"
#include "gfx/gfx.h"
#include "ui/ui.h"
#include "ui/ui_transform.h"

#include "cube_hud_screen.h"
#include "cube_menu_screen.h"

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

    /* Worst case: the widest fps_line string "%.1f fps" can produce. */
    const cube_hud_screen_state_t state = {
        .fps_value = 999.9,
        .fps_box_x_override = -1,
    };

    const input_t input = {0};
    ui_begin(&input);
    cube_hud_screen_draw(ui_context(), &state);
    assert_budget("cube HUD", end_and_measure());
}

static void
test_menu_screen_command_list_fits_budget(void) {
    fixture();

    const cube_menu_screen_state_t state = {
        .partial_updates_on = true,
        .band_mode_on = true,
    };

    const input_t input = {0};
    ui_begin(&input);
    cube_menu_screen_draw(ui_context(), &state);
    assert_budget("cube menu", end_and_measure());
}

void
run_cube_command_list_budget_suite(void) {
    RUN_TEST(test_hud_screen_command_list_fits_budget);
    RUN_TEST(test_menu_screen_command_list_fits_budget);
}

SUITE_REGISTER(run_cube_command_list_budget_suite);
