/*
 * sand_menu_render_host - the sand app's title and options screens, drawn
 * by their real code through the real ui and gfx layers, no board.
 *
 * Only the menu is linked, not app_sand.c: the screens take their option
 * names as data, so the tables below stand in for the app's own.
 */

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "gfx/gfx.h"
#include "render_host.h"
#include "ui/ui.h"
#include "ui/ui_transform.h"

#include "apps/sand/sand_menu.h"
#include "apps/sand/sand_mode_swatches.h"
#include "apps/sand/sand_palette256.h"
#include "apps/sand/ui/options_screen.h"
#include "apps/sand/ui/title_screen.h"

#define BACKGROUND 0x0A0C14

static const char* const QUALITY_NAMES[] = {"ULTRA", "HIGH", "NORMAL", "LOW", "VERY LOW"};
static const char* const DITHER_NAMES[] = {"NONE", "CELL CHECKER", "CELL BAYER2", "PIXEL CHECKER2", "PIXEL BAYER4"};

static sand_mode_swatch_t mode_swatches[SAND_COLOUR_MODE_COUNT];

static const options_screen_labels_t LABELS = {
    .quality_names = QUALITY_NAMES,
    .quality_count = 5,
    .dither_names = DITHER_NAMES,
    .dither_count = 5,
    .mode_swatches = mode_swatches,
};

static sand_menu_t menu;
static sand_options_t committed;
static bool show_options;
static sand_colour_mode_t colour = SAND_COLOUR_256;
static bool pending;
static bool open_dither;
static ui_transform_t transform;

int
display_shell_quarter(void) {
    return 0;
}

static bool
options(int argc, char** argv) {
    for (int i = 0; i < argc; i++) {
        if (strcmp(argv[i], "--options") == 0) {
            show_options = true;
        } else if (strcmp(argv[i], "--sixteen") == 0) {
            colour = SAND_COLOUR_16;
        } else if (strcmp(argv[i], "--pending") == 0) {
            pending = true;
        } else if (strcmp(argv[i], "--open-dither") == 0) {
            open_dither = true;
        } else {
            fprintf(stderr, "sand_menu_render_host: unknown option %s\n", argv[i]);
            return false;
        }
    }
    return true;
}

static bool
setup(int quarter) {
    sand_mode_swatches(sand_dither_none_lut, sand_palette256_lut, GFX_INDEXED_PALETTE_SIZE, SAND_PALETTE_UI_ENTRIES,
                       mode_swatches);
    ui_init();
    transform = ui_transform_quarter_turn(quarter, GFX_WIDTH, GFX_HEIGHT);
    ui_set_transform(transform);
    committed = (sand_options_t){.quality = 2, .color = colour, .dither = 2};
    sand_menu_init(&menu);
    if (show_options) {
        sand_menu_title_clicked(&menu, SAND_TITLE_OPTIONS, committed);
    }
    if (pending) {
        menu.draft.quality = 1;
        menu.draft.color = SAND_COLOUR_FULL;
    }
    return true;
}

/* A tap on the DITHER dropdown over frames 2-7, placed from the canvas
 * layout and turned onto the panel, where touch arrives: the harness's own
 * touch steps are fixed before the canvas is known. */
static input_t
dither_tap(int index) {
    options_screen_layout_t lay;
    options_screen_layout(ui_width(), ui_height(), &lay);
    input_t in = {0};
    ui_transform_point(transform, lay.dither.x + lay.dither.w / 2, lay.dither.y + lay.dither.h / 2, &in.x, &in.y);
    in.down = index >= 2 && index < 7;
    in.pressed = index == 2;
    in.released = index == 7;
    return in;
}

static void
draw(const render_frame_t* frame) {
    const input_t input = open_dither ? dither_tap(frame->index) : frame->input;
    ui_begin(&input);
    if (menu.screen == SAND_MENU_OPTIONS) {
        options_screen_draw(ui_context(), &menu, committed, &LABELS);
    } else {
        title_screen_draw(ui_context());
    }
    ui_end(BACKGROUND);
}

const render_scene_t render_scene = {
    .name = "sand_menu",
    .quarter = 0,
    .frames = 3,
    .dt_ms = 16,
    .options = options,
    .setup = setup,
    .draw = draw,
};
