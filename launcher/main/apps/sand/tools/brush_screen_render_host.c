/*
 * brush_screen_render_host - the sand app's two overlay panels, the brush
 * screen and the material palette, drawn by their real code through the
 * real ui and gfx layers, no board.
 *
 * A flat fill stands in for the frozen, dimmed sandbox both panels are drawn
 * over; the brush cell, mode and radius are fixed below.
 */

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "apps/sand/ui/brush_screen.h"
#include "apps/sand/ui/palette_screen.h"
#include "apps/sand/ui/sand_icons.h"
#include "display/display.h"
#include "draw/draw.h"
#include "gfx/gfx.h"
#include "image/pixel/rgb565.h"
#include "render_host.h"
#include "sand_brushes.h"
#include "sand_ui.h"
#include "ui/layout/ui_transform.h"
#include "ui/ui.h"

/* The longest name, the worst case brush_screen.c's name-shrink loop
 * handles, at the middle radius. */
#define PREVIEW_MODE       SAND_MODE_PAINT
#define PREVIEW_RADIUS_PX  ((SAND_UI_RADIUS_MIN + SAND_UI_RADIUS_MAX) / 2)
/* Every third palette tile is a source, so the badge shows checked and not. */
#define PREVIEW_SPAWN_STEP 3

static uint8_t modes[SAND_BRUSH_COUNT];
static sand_ui_t ui;
static bool show_palette;

int
display_quarter_now(void) {
    return DISPLAY_PORTRAIT;
}

static bool
options(int argc, char** argv) {
    for (int i = 0; i < argc; i++) {
        if (strcmp(argv[i], "--palette") == 0) {
            show_palette = true;
        } else {
            fprintf(stderr, "brush_screen_render_host: unknown option %s\n", argv[i]);
            return false;
        }
    }
    return true;
}

static bool
setup(int quarter) {
    ui_init();
    (void)ui_icon_set_load(&sand_icon_set);
    ui_set_transform(ui_transform_quarter_turn(quarter, GFX_WIDTH, GFX_HEIGHT));
    for (int i = 0; i < SAND_BRUSH_COUNT; i++) {
        modes[i] = i % PREVIEW_SPAWN_STEP == 0 ? BRUSH_SPAWN : BRUSH_POUR;
    }
    ui = (sand_ui_t){
        .brushes = sand_brushes,
        .modes = modes,
        .brush_count = SAND_BRUSH_COUNT,
        .brush = sand_brush_longest_name(),
        .screen = show_palette ? SAND_UI_PALETTE : SAND_UI_BRUSH,
        .mode = PREVIEW_MODE,
    };
    for (int i = 0; i < SAND_MODE_COUNT; i++) {
        ui.radius_px[i] = PREVIEW_RADIUS_PX;
    }
    return true;
}

static void
draw(const provider_frame_t* frame) {
    draw_clear(RGB565BE(0x000000));
    ui_invalidate();
    ui_begin(&frame->input);
    if (show_palette) {
        palette_screen_draw(&ui);
    } else {
        brush_screen_draw(&ui);
    }
    ui_end(UI_NO_BACKGROUND);
}

const frame_provider_t brush_screen_provider = {
    .name = "brush_screen",
    .quarter = 0,
    .frames = 3,
    .dt_ms = 16,
    .options = options,
    .setup = setup,
    .draw = draw,
};
