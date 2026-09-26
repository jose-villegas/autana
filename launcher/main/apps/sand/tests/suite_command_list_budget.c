/*
 * Portable suite: measures the peak bytes each of this app's microui
 * screens' command list reaches, driving the REAL palette_screen_draw()/
 * brush_screen_draw()/title_screen_draw()/options_screen_draw() against a
 * real microui (ui_init() + ui_begin(), the same calls app_sand.c makes)
 * rather than a
 * hand-mirrored reconstruction - see docs/Building-a-Screen.md.
 *
 * production's own brushes[] lives in app_sand.c (the hardware entry
 * point, not host-compiled), so test_brushes[] below is a representative
 * copy of it FOR SIZING ONLY - suite_palette.c/suite_sand_ui.c/
 * suite_brush_screen.c cover the real behaviour. Every scenario below picks
 * the worst case a real visit can reach (tile 0 selected AND every
 * emit-eligible tile flagged BRUSH_SPAWN, the longest name in each option
 * table, the DITHER list open) rather than whatever a fresh sand_ui_t
 * happens to zero-initialize to.
 */

#include <string.h>

#include "suites.h"
#include "unity.h"

#include "app.h"
#include "gfx/gfx.h"
#include "ui/ui.h"
#include "ui/ui_transform.h"

#include "apps/sand/material.h"
#include "apps/sand/sand_ui.h"
#include "apps/sand/ui/brush_screen.h"
#include "apps/sand/ui/options_screen.h"
#include "apps/sand/ui/palette_screen.h"
#include "apps/sand/ui/title_screen.h"

/* The host's larger command list checks rendering growth; device/QEMU enforces
 * the 8 KiB RAM limit. A fifth screen or a busier icon still has this many bytes of
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

static const sand_brush_t test_brushes[] = {
    SAND_BRUSH_SOLID(CELL_MAKE(MAT_SAND, 0)),  SAND_BRUSH_SOLID(CELL_MAKE(MAT_WATER, 0)),
    SAND_BRUSH_SOLID(CELL_MAKE(MAT_STONE, 0)), SAND_BRUSH_SOLID(CELL_MAKE(MAT_GAS, 0)),
    SAND_BRUSH_SOLID(CELL_MAKE(MAT_FIRE, 0)),  SAND_BRUSH_SOLID(CELL_MAKE(MAT_WOOD, 0)),
    SAND_BRUSH_SOLID(CELL_MAKE(MAT_OIL, 0)),   SAND_BRUSH_SOLID(CELL_MAKE(MAT_LAVA, 0)),
    SAND_BRUSH_SOLID(CELL_MAKE(MAT_ACID, 0)),  SAND_BRUSH_SOLID(CELL_MAKE(MAT_GLASS, 0)),
    SAND_BRUSH_SOLID(CELL_MAKE(MAT_SNOW, 0)),  SAND_BRUSH_SOLID(CELL_MAKE(MAT_DIRT, 0)),
    SAND_BRUSH_SOLID(MATX(MATX_ICE)),          SAND_BRUSH_SPARSE(MATX(MATX_PLANT), SAND_BRUSH_SHARE_PLANT),
    SAND_BRUSH_SOLID(GUNPOWDER_CELL(0)),
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
test_title_screen_command_list_fits_budget(void) {
    fixture();

    const input_t input = {0};
    ui_begin(&input);
    title_screen_draw(ui_context());
    assert_budget("sand title", end_and_measure());
}

/* Shaped like the app's, for layout, taps and command-list size; the
 * colours themselves are suite_sand_mode_swatches.c's. */
static const sand_mode_swatch_t TEST_MODE_SWATCHES[SAND_COLOUR_MODE_COUNT] = {
    [SAND_COLOUR_FULL] = {.cols = SAND_SWATCH_FULL_BANDS, .rows = 1},
    [SAND_COLOUR_256] = {.cols = SAND_SWATCH_256_COLS, .rows = SAND_SWATCH_ROWS},
    [SAND_COLOUR_16] = {.cols = SAND_SWATCH_16_COLS, .rows = SAND_SWATCH_ROWS},
};

static const char* const TEST_QUALITY_NAMES[] = {"ULTRA", "HIGH", "NORMAL", "LOW", "VERY LOW"};
static const char* const TEST_DITHER_NAMES[] = {"NONE", "CELL CHECKER", "CELL BAYER2", "PIXEL CHECKER2",
                                                "PIXEL BAYER4"};

static void
test_options_screen_command_list_fits_budget(void) {
    fixture();

    const options_screen_labels_t labels = {
        .quality_names = TEST_QUALITY_NAMES,
        .quality_count = 5,
        .dither_names = TEST_DITHER_NAMES,
        .dither_count = 5,
        .mode_swatches = TEST_MODE_SWATCHES,
    };
    sand_menu_t menu;
    const sand_options_t committed = {.quality = 4, .color = SAND_COLOUR_16, .dither = 3};
    sand_menu_init(&menu);
    sand_menu_title_clicked(&menu, SAND_TITLE_OPTIONS, committed);
    menu.draft.quality = 0;

    const input_t input = {0};
    ui_begin(&input);
    options_screen_draw(ui_context(), &menu, committed, &labels);
    assert_budget("sand options", end_and_measure());
}

/* The dither list open: every row, swatch and all, on top of the screen.
 * Portrait, so a tap lands where the layout says with no turn between. */
static void
test_options_screen_with_its_dither_list_open_fits_budget(void) {
    ui_init();
    ui_set_transform(ui_transform_identity());

    const options_screen_labels_t labels = {
        .quality_names = TEST_QUALITY_NAMES,
        .quality_count = 5,
        .dither_names = TEST_DITHER_NAMES,
        .dither_count = 5,
        .mode_swatches = TEST_MODE_SWATCHES,
    };
    sand_menu_t menu;
    const sand_options_t committed = {.quality = 4, .color = SAND_COLOUR_16, .dither = 3};
    sand_menu_init(&menu);
    sand_menu_title_clicked(&menu, SAND_TITLE_OPTIONS, committed);

    options_screen_layout_t lay;
    options_screen_layout(ui_width(), ui_height(), &lay);
    const int x = lay.dither.x + lay.dither.w / 2;
    const int y = lay.dither.y + lay.dither.h / 2;
    const input_t steps[] = {
        {0},
        {0},
        {.down = true, .pressed = true, .x = x, .y = y},
        {.down = true, .x = x, .y = y},
        {.down = true, .x = x, .y = y},
        {.down = true, .x = x, .y = y},
        {.released = true, .x = x, .y = y},
        {0},
    };
    for (size_t i = 0; i < sizeof steps / sizeof steps[0]; i++) {
        ui_begin(&steps[i]);
        options_screen_draw(ui_context(), &menu, committed, &labels);
        mu_end(ui_context());
    }

    const input_t idle = {0};
    ui_begin(&idle);
    options_screen_draw(ui_context(), &menu, committed, &labels);
    const int used = end_and_measure();
    TEST_ASSERT_GREATER_THAN_INT_MESSAGE(1, ui_context()->root_list.idx,
                                         "the list must actually be open for this to measure it");
    assert_budget("sand options, dither list open", used);
}

void
run_sand_command_list_budget_suite(void) {
    RUN_TEST(test_palette_screen_command_list_fits_budget);
    RUN_TEST(test_brush_screen_command_list_fits_budget);
    RUN_TEST(test_title_screen_command_list_fits_budget);
    RUN_TEST(test_options_screen_command_list_fits_budget);
    RUN_TEST(test_options_screen_with_its_dither_list_open_fits_budget);
}

SUITE_REGISTER(run_sand_command_list_budget_suite);
