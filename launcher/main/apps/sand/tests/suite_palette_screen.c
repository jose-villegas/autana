/*
 * Portable suite: palette_screen - real taps through the face on the
 * material picker (one tile selects exactly its brush, only on release, only
 * if the press began on it), and, on the host, what each tile looks like:
 * the selected tile's sunken bevel, the label ink, the source badge's check.
 */

#include <stdbool.h>
#include <stdint.h>

#include "suites.h"
#include "testkit/cleanup.h"
#include "unity.h"

#include "core/memory.h"
#include "gfx/gfx.h"
#include "gfx/target/gfx_color.h"
#include "input/input.h"
#include "ui/ui.h"
#include "ui_probe.h"

#include "apps/sand/material.h"
#include "apps/sand/material_palette.h"
#include "apps/sand/palette.h"
#include "apps/sand/sand_brushes.h"
#include "apps/sand/ui/palette_screen.h"
#include "apps/sand/ui/sand_theme.h"
#include "image/pixel/rgb565.h"

#define TOUCH_HOLD_FRAMES 4

static uint8_t* modes;

static void
cleanup(void) {
#ifndef ESP_PLATFORM
    ui_probe_widgets_stop();
#endif
    memory_free(modes);
    modes = NULL;
}

static sand_ui_t
fixture(bool landscape, int selected) {
    suite_set_test_cleanup(cleanup);
    modes = memory_alloc(SAND_BRUSH_COUNT * sizeof *modes, MEMORY_PSRAM);
    TEST_ASSERT_NOT_NULL(modes);
    for (int i = 0; i < SAND_BRUSH_COUNT; i++) {
        modes[i] = BRUSH_POUR;
    }
    ui_init();
    ui_set_transform(ui_transform_quarter_turn(landscape ? 1 : 0, GFX_WIDTH, GFX_HEIGHT));
    sand_ui_t ui = {0};
    ui.brushes = sand_brushes;
    ui.modes = modes;
    ui.brush_count = SAND_BRUSH_COUNT;
    ui.screen = SAND_UI_PALETTE;
    ui.brush = selected;
    ui.mode = SAND_MODE_ERASE;
    return ui;
}

static void
palette_frame(sand_ui_t* ui, bool landscape, bool down, bool pressed, bool released, vec2i_t at) {
    const affine2x_t transform = ui_transform_quarter_turn(landscape ? 1 : 0, GFX_WIDTH, GFX_HEIGHT);
    const vec2i_t pixel = affine2x_map_pixel(&transform, at);
    const input_t in = {.down = down, .pressed = pressed, .released = released, .x = pixel.x, .y = pixel.y};
    ui_begin(&in);
    palette_screen_draw(ui);
    ui_probe_end_frame();
}

static void
settle(sand_ui_t* ui, bool landscape) {
    palette_frame(ui, landscape, false, false, false, (vec2i_t){0, 0});
    palette_frame(ui, landscape, false, false, false, (vec2i_t){0, 0});
}

static recti_t
cell(int index) {
    int x;
    int y;
    int w;
    int h;
    palette_tile_rect(index, SAND_BRUSH_COUNT, palette_cols(ui_width()), ui_width(), ui_height(), &x, &y, &w, &h);
    return (recti_t){x, y, w, h};
}

static vec2i_t
centre(recti_t r) {
    return (vec2i_t){r.x + (r.w / 2), r.y + (r.h / 2)};
}

/* Presses at `from`, holds and releases at `to`, asserting the selection
 * is untouched until the release frame. */
static void
drag_tap(sand_ui_t* ui, bool landscape, vec2i_t from, vec2i_t to) {
    const int start = ui->brush;
    palette_frame(ui, landscape, true, true, false, from);
    TEST_ASSERT_EQUAL_INT_MESSAGE(start, ui->brush, "nothing may fire on the press frame");
    for (int i = 0; i < TOUCH_HOLD_FRAMES; i++) {
        palette_frame(ui, landscape, true, false, false, to);
        TEST_ASSERT_EQUAL_INT_MESSAGE(start, ui->brush, "nothing may fire while the finger is held");
    }
    palette_frame(ui, landscape, false, false, true, to);
}

static void
assert_a_tap_selects_exactly_its_tile(bool landscape) {
    for (int i = 0; i < SAND_BRUSH_COUNT; i++) {
        const int other = (i + 1) % SAND_BRUSH_COUNT;
        sand_ui_t ui = fixture(landscape, other);
        settle(&ui, landscape);
        const vec2i_t at = centre(cell(i));
        drag_tap(&ui, landscape, at, at);
        TEST_ASSERT_EQUAL_INT_MESSAGE(i, ui.brush, "a tap on tile i must select brush i");
        TEST_ASSERT_EQUAL_INT_MESSAGE(SAND_MODE_PAINT, ui.mode, "selecting a brush resets the mode to paint");
        cleanup();
    }
}

static void
test_a_tap_selects_exactly_its_tile_portrait(void) {
    assert_a_tap_selects_exactly_its_tile(false);
}

static void
test_a_tap_selects_exactly_its_tile_landscape(void) {
    assert_a_tap_selects_exactly_its_tile(true);
}

/* One app pass: sand_ui_step() reads the input before the open panel draws
 * the same frame, as app_sand.c orders them. */
static void
app_frame(sand_ui_t* ui, input_t in) {
    const affine2x_t transform = ui_transform_quarter_turn(0, GFX_WIDTH, GFX_HEIGHT);
    const vec2i_t pixel = affine2x_map_pixel(&transform, (vec2i_t){in.x, in.y});
    in.x = pixel.x;
    in.y = pixel.y;
    (void)sand_ui_step(ui, &in);
    if (ui->screen == SAND_UI_PALETTE) {
        ui_begin(&in);
        palette_screen_draw(ui);
        ui_probe_end_frame();
    }
}

/* A finger landing on a tile as BOOT opens the palette is a pour's touch,
 * not a pick: its release, delivered by the UI after sand_ui_step(), must
 * still be swallowed. The next real tap selects. */
static void
test_a_touch_that_opens_with_the_palette_selects_nothing_on_release(void) {
    sand_ui_t ui = fixture(false, 0);
    ui.screen = SAND_UI_RUNNING;
    const vec2i_t at = centre(cell(1));
    app_frame(&ui, (input_t){.down = true, .pressed = true, .boot.released = true, .x = at.x, .y = at.y});
    TEST_ASSERT_EQUAL_INT(SAND_UI_PALETTE, ui.screen);
    for (int i = 0; i < TOUCH_HOLD_FRAMES; i++) {
        app_frame(&ui, (input_t){.down = true, .x = at.x, .y = at.y});
    }
    app_frame(&ui, (input_t){.released = true, .x = at.x, .y = at.y});
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, ui.brush, "the opening touch's release must be swallowed");
    app_frame(&ui, (input_t){0});
    TEST_ASSERT_FALSE(ui.swallow_release);

    app_frame(&ui, (input_t){.down = true, .pressed = true, .x = at.x, .y = at.y});
    app_frame(&ui, (input_t){.released = true, .x = at.x, .y = at.y});
    TEST_ASSERT_EQUAL_INT_MESSAGE(1, ui.brush, "the next real tap selects");
}

static void
test_the_second_tile_selects_a_different_brush_than_the_first(void) {
    sand_ui_t ui = fixture(false, SAND_BRUSH_COUNT - 1);
    settle(&ui, false);
    drag_tap(&ui, false, centre(cell(0)), centre(cell(0)));
    const int first = ui.brush;
    drag_tap(&ui, false, centre(cell(1)), centre(cell(1)));
    TEST_ASSERT_EQUAL_INT(0, first);
    TEST_ASSERT_EQUAL_INT(1, ui.brush);
}

static void
test_a_press_that_slides_off_its_tile_selects_nothing(void) {
    sand_ui_t ui = fixture(false, 0);
    settle(&ui, false);
    const recti_t first = cell(1);
    const recti_t next = cell(2);

    drag_tap(&ui, false, centre(first), centre(next));
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, ui.brush, "a press on one tile released on another picks neither");

    const vec2i_t off_screen = {-1, -1};
    drag_tap(&ui, false, centre(first), off_screen);
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, ui.brush, "a press slid off every tile picks nothing");
}

static void
test_a_tap_in_the_grout_between_tiles_selects_the_nearest_tile(void) {
    sand_ui_t ui = fixture(false, 0);
    settle(&ui, false);

    /* The grout is split evenly by the cell edge, so a pixel inside the edge
     * is nearer to that cell's tile than to its neighbour's. */
    const recti_t left = cell(1);
    const vec2i_t beside_left = {left.x + left.w - 1, left.y + (left.h / 2)};
    drag_tap(&ui, false, beside_left, beside_left);
    TEST_ASSERT_EQUAL_INT_MESSAGE(1, ui.brush, "the grout beside a tile picks that tile");

    drag_tap(&ui, false, centre(cell(0)), centre(cell(0)));
    const recti_t above = cell(1);
    const vec2i_t under_above = {above.x + (above.w / 2), above.y + above.h - 1};
    drag_tap(&ui, false, under_above, under_above);
    TEST_ASSERT_EQUAL_INT_MESSAGE(1, ui.brush, "the grout under a tile picks that tile");
}

#ifndef ESP_PLATFORM
/* What the screen is specified to ink, as 0xRRGGBB. */
#define LABEL_RGB       0x000000
#define BADGE_CHECK_RGB 0x141414

static void
visual_fixture(sand_ui_t* ui) {
    ui_probe_widgets_start();
    palette_frame(ui, false, false, false, false, (vec2i_t){0, 0});
    palette_frame(ui, false, false, false, false, (vec2i_t){0, 0});
}

static recti_t
tile_of(int index) {
    recti_t r = {0};
    TEST_ASSERT_TRUE_MESSAGE(ui_probe_widget(material_name(sand_brushes[index].cell), &r), "tile was not drawn");
    return r;
}

static uint32_t
face_of(int index) {
    return rgb565be_to_rgb888(material_brush_color(sand_brushes[index].cell));
}

static void
test_only_the_selected_tile_is_sunken(void) {
    for (int selected = 0; selected < SAND_BRUSH_COUNT; selected += SAND_BRUSH_COUNT - 1) {
        sand_ui_t ui = fixture(false, selected);
        visual_fixture(&ui);
        for (int i = 0; i < SAND_BRUSH_COUNT; i++) {
            const int sunken = ui_probe_bevel_index(tile_of(i), face_of(i), true);
            const int raised = ui_probe_bevel_index(tile_of(i), face_of(i), false);
            TEST_ASSERT_GREATER_OR_EQUAL_INT_MESSAGE(0, raised, "every tile keeps its raised bezel");
            if (i == selected) {
                TEST_ASSERT_GREATER_THAN_INT_MESSAGE(raised, sunken, "the sunken bevel is drawn over the raised one");
            } else {
                TEST_ASSERT_LESS_THAN_INT_MESSAGE(0, sunken, "only the selected tile is sunken");
            }
        }
        cleanup();
    }
}

static void
test_tile_labels_are_inked_black_not_the_theme_text(void) {
    TEST_ASSERT_NOT_EQUAL_UINT32_MESSAGE(LABEL_RGB, sand_ui_theme.text, "the theme text would hide a wrong ink");
    sand_ui_t ui = fixture(false, 0);
    visual_fixture(&ui);
    for (int i = 0; i < SAND_BRUSH_COUNT; i++) {
        uint32_t rgb = 0;
        TEST_ASSERT_TRUE(ui_probe_text_rgb(material_name(sand_brushes[i].cell), &rgb));
        TEST_ASSERT_EQUAL_HEX32_MESSAGE(LABEL_RGB, rgb, material_name(sand_brushes[i].cell));
    }
}

static void
test_the_badge_check_is_inked_in_the_badge_border_colour_for_spawn_tiles_only(void) {
    TEST_ASSERT_NOT_EQUAL_UINT32(BADGE_CHECK_RGB, sand_ui_theme.text);
    TEST_ASSERT_NOT_EQUAL_UINT32(BADGE_CHECK_RGB, LABEL_RGB);

    sand_ui_t pour = fixture(false, 0);
    visual_fixture(&pour);
    for (int i = 0; i < SAND_BRUSH_COUNT; i++) {
        uint32_t rgb = 0;
        TEST_ASSERT_FALSE_MESSAGE(ui_probe_icon_rgb(tile_of(i), &rgb), "a pour tile draws no check");
    }
    cleanup();

    sand_ui_t spawn = fixture(false, 0);
    for (int i = 0; i < SAND_BRUSH_COUNT; i++) {
        modes[i] = BRUSH_SPAWN;
    }
    visual_fixture(&spawn);
    for (int i = 0; i < SAND_BRUSH_COUNT; i++) {
        uint32_t rgb = 0;
        const bool drawn = ui_probe_icon_rgb(tile_of(i), &rgb);
        TEST_ASSERT_EQUAL_INT_MESSAGE(material_can_emit(sand_brushes[i].cell), drawn,
                                      "only a material that can emit carries a badge, check and all");
        if (drawn) {
            TEST_ASSERT_EQUAL_HEX32(BADGE_CHECK_RGB, rgb);
        }
    }
}

#endif

void
run_palette_screen_suite(void) {
    RUN_TEST(test_a_tap_selects_exactly_its_tile_portrait);
    RUN_TEST(test_a_tap_selects_exactly_its_tile_landscape);
    RUN_TEST(test_the_second_tile_selects_a_different_brush_than_the_first);
    RUN_TEST(test_a_touch_that_opens_with_the_palette_selects_nothing_on_release);
    RUN_TEST(test_a_press_that_slides_off_its_tile_selects_nothing);
    RUN_TEST(test_a_tap_in_the_grout_between_tiles_selects_the_nearest_tile);
#ifndef ESP_PLATFORM
    RUN_TEST(test_only_the_selected_tile_is_sunken);
    RUN_TEST(test_tile_labels_are_inked_black_not_the_theme_text);
    RUN_TEST(test_the_badge_check_is_inked_in_the_badge_border_colour_for_spawn_tiles_only);
#endif
}

SUITE_REGISTER(run_palette_screen_suite);
