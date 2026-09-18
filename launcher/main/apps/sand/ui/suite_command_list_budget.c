/*
 * Portable suite: measures the peak bytes each of this app's three microui
 * screens' command list reaches, driving the REAL palette_screen_draw()/
 * brush_screen_draw()/sand_menu_screen_draw() against a real microui
 * (ui_init() + ui_begin(), the same calls app_sand.c makes) rather than a
 * hand-mirrored reconstruction - see docs/Building-a-Screen.md.
 *
 * production's own brushes[] lives in app_sand.c (the hardware entry
 * point, not host-compiled), so test_brushes[] below is a representative
 * copy of it FOR SIZING ONLY - suite_palette.c/suite_sand_ui.c/
 * suite_brush_screen.c cover the real behaviour. Every scenario below picks
 * the worst case a real visit can reach (tile 0 selected AND every
 * emit-eligible tile flagged BRUSH_SPAWN, the longest name in each option
 * table, the DITHER row shown) rather than whatever a fresh sand_ui_t
 * happens to zero-initialize to.
 */

#include <string.h>

#include "suites.h"
#include "unity.h"

#include "app.h"
#include "gfx/gfx.h"
#include "ui/ui.h"
#include "ui/ui_transform.h"

#include "../material.h"
#include "../sand_ui.h"
#include "brush_screen.h"
#include "palette_screen.h"
#include "sand_menu_screen.h"

/* A fifth screen or a busier icon still has this many bytes of
 * MU_COMMANDLIST_SIZE left to grow into before any screen's own peak
 * reaches the ceiling microui.c asserts against. */
#define COMMANDLIST_HEADROOM_BYTES 2048
#define COMMANDLIST_BUDGET         (MU_COMMANDLIST_SIZE - COMMANDLIST_HEADROOM_BYTES)

/* Landscape (448x368 logical) - this app's shipping orientation, and what
 * suite_brush_screen.c/suite_palette.c already assert every rect below
 * fits inside. */
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

static const cell_t test_brushes[] = {
    CELL_MAKE(MAT_SAND, 0), CELL_MAKE(MAT_WATER, 0), CELL_MAKE(MAT_STONE, 0), CELL_MAKE(MAT_GAS, 0),
    CELL_MAKE(MAT_FIRE, 0), CELL_MAKE(MAT_WOOD, 0),  CELL_MAKE(MAT_OIL, 0),   CELL_MAKE(MAT_LAVA, 0),
    CELL_MAKE(MAT_ACID, 0), CELL_MAKE(MAT_GLASS, 0), CELL_MAKE(MAT_SNOW, 0),  CELL_MAKE(MAT_DIRT, 0),
    MATX(MATX_ICE),         MATX(MATX_PLANT),        GUNPOWDER_CELL(0),
};
#define TEST_BRUSH_COUNT ((int)(sizeof(test_brushes) / sizeof(test_brushes[0])))

static void
test_palette_screen_command_list_fits_budget(void) {
    fixture();

    static uint8_t modes[TEST_BRUSH_COUNT];
    memset(modes, BRUSH_SPAWN, sizeof modes);

    sand_ui_t ui = {
        .brushes = test_brushes,
        .modes = modes,
        .brush_count = TEST_BRUSH_COUNT,
        .brush = 0,
    };

    const input_t input = {0};
    ui_begin(&input);
    palette_screen_draw(ui_context(), &ui);
    assert_budget("sand palette", end_and_measure());
}

static void
test_brush_screen_command_list_fits_budget(void) {
    fixture();

    static uint8_t modes[TEST_BRUSH_COUNT];
    memset(modes, 0, sizeof modes);

    sand_ui_t ui = {
        .brushes = test_brushes,
        .modes = modes,
        .brush_count = TEST_BRUSH_COUNT,
        .brush = TEST_BRUSH_COUNT - 1, /* "Gunpowder" - the longest material name */
        .mode = SAND_MODE_DETONATE,
    };
    for (int i = 0; i < SAND_MODE_COUNT; i++) {
        ui.radius_px[i] = SAND_UI_RADIUS_MAX;
    }

    const input_t input = {0};
    ui_begin(&input);
    brush_screen_draw(ui_context(), &ui);
    assert_budget("sand brush", end_and_measure());
}

static void
test_menu_screen_command_list_fits_budget(void) {
    fixture();

    const sand_menu_screen_state_t state = {
        .quality = "QUALITY: VERY LOW",
        .color = "COLOUR: FULL",
        .dither = "DITHER: PIXEL CHECKER2",
        .show_dither = true,
    };

    const input_t input = {0};
    ui_begin(&input);
    sand_menu_screen_draw(ui_context(), &state, 16);
    assert_budget("sand menu", end_and_measure());
}

void
run_sand_command_list_budget_suite(void) {
    RUN_TEST(test_palette_screen_command_list_fits_budget);
    RUN_TEST(test_brush_screen_command_list_fits_budget);
    RUN_TEST(test_menu_screen_command_list_fits_budget);
}

SUITE_REGISTER(run_sand_command_list_budget_suite);
