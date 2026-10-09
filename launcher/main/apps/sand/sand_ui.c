#include "sand_ui.h"

/* The selection `screen`'s label confirms, see sand_ui_t.opened. */
static sand_ui_selection_t
selection(const sand_ui_t* ui, sand_ui_screen_t screen) {
    if (screen == SAND_UI_PALETTE) {
        return (sand_ui_selection_t){ui->brush, ui->modes[ui->brush]};
    }
    return (sand_ui_selection_t){(int)ui->mode, ui->radius_px[ui->mode]};
}

/* Opens the palette (BOOT) or brush screen (PWR). Arms `swallow_release`
 * only when a finger is already down: a pour in progress would otherwise
 * release onto a tile. Arming it with no finger down would eat the player's first real tap. */
static unsigned
open_panel(sand_ui_t* ui, sand_ui_screen_t screen, bool touch_in_progress, unsigned opened) {
    ui->screen = screen;
    ui->swallow_release = touch_in_progress;
    ui->opened = selection(ui, screen);
    return opened;
}

/* Closes the open panel; app_sand.c repaints and resets the step
 * accumulators. The label is asked for only if the selection changed while
 * open, so closing says nothing when there is nothing to confirm. */
static unsigned
close_panel(sand_ui_t* ui, unsigned closed) {
    const sand_ui_selection_t now = selection(ui, ui->screen);
    ui->screen = SAND_UI_RUNNING;
    if (now.choice != ui->opened.choice || now.setting != ui->opened.setting) {
        closed |= SAND_UI_SHOW_LABEL;
    }
    return closed;
}

/* While a panel is open: `close` is its closing edge, never the opening
 * one, as sand_ui_step() reads the screen once. Taps are decided elsewhere
 * (sand_ui.h); this disarms `swallow_release` once the finger is up. */
static unsigned
handle_panel_input(sand_ui_t* ui, const input_t* input, bool close, unsigned closed) {
    if (close) {
        return close_panel(ui, closed);
    }

    if (ui->swallow_release && !input->down) {
        ui->swallow_release = false;
    }

    return 0;
}

/* What a click on palette tile `index` means, see this function's own
 * doc comment in sand_ui.h for the full contract, and the "WHO
 * HIT-TESTS AND WHO DECIDES" note there for why the hit-test producing
 * `index` belongs to microui, not this module. draw_palette() in
 * app_sand.c is the only caller, from inside its own per-tile mu_button()
 * loop, so there is no "index hits nothing" branch here: an index this
 * function is ever handed already named a real tile. */
unsigned
sand_ui_tile_clicked(sand_ui_t* ui, int index) {
    /* Swallow the first click after opening with a finger already down;
     * handle_panel_input() disarms this once that finger lifts. See
     * sand_ui_t.swallow_release. */
    if (ui->swallow_release) {
        return 0;
    }

    if (index == ui->brush) {
        /* Tapping the ALREADY-selected tile toggles its mode instead of
         * re-selecting it - selection state has nothing left to change,
         * so a second tap has to mean something else. Only if the
         * material is eligible to be a source at all: an ineligible tile
         * has no mode to toggle into, so this does nothing rather than
         * silently flip a bit nothing ever reads (see
         * material_can_emit()). */
        if (!material_can_emit(ui->brushes[ui->brush].cell)) {
            return 0;
        }
        ui->modes[ui->brush] = (ui->modes[ui->brush] == BRUSH_POUR) ? BRUSH_SPAWN : BRUSH_POUR;
    } else {
        /* A different tile: select it. Its own remembered mode is left
         * exactly as it was - only `mode` resets on selection, the same as
         * always (ERASE and DETONATE alike: choosing a material means you
         * want to place it, not erase or blow up whatever is already
         * there). */
        ui->brush = index;
        ui->mode = SAND_MODE_PAINT;
    }

    return SAND_UI_REDRAW_PALETTE;
}

/* What a tap on brush-mode segment `index` means, see this function's own
 * doc comment in sand_ui.h for the full contract, and "WHO HIT-TESTS AND
 * WHO DECIDES" for why the hit-test producing `index` is not this
 * module's job. `index` lines up with sand_mode_t directly - see
 * brush_screen_segment_t's own comment on the ordering the two share. */
unsigned
sand_ui_mode_clicked(sand_ui_t* ui, int index) {
    if (ui->swallow_release) {
        return 0;
    }

    if ((sand_mode_t)index == ui->mode) {
        return 0;
    }

    ui->mode = (sand_mode_t)index;
    return SAND_UI_REDRAW_BRUSH;
}

/* Clamps to [SAND_UI_RADIUS_MIN, SAND_UI_RADIUS_MAX] and stores into the
 * CURRENT mode's slot only - see sand_ui_t.radius_px. */
void
sand_ui_set_radius(sand_ui_t* ui, uint8_t radius_px) {
    if (radius_px < SAND_UI_RADIUS_MIN) {
        radius_px = SAND_UI_RADIUS_MIN;
    } else if (radius_px > SAND_UI_RADIUS_MAX) {
        radius_px = SAND_UI_RADIUS_MAX;
    }

    ui->radius_px[ui->mode] = radius_px;
}

uint8_t
sand_ui_radius(const sand_ui_t* ui) {
    return ui->radius_px[ui->mode];
}

/* Only reachable while SAND_UI_RUNNING. BOOT's release opens the palette,
 * PWR's press the brush screen. Neither reads `.held`: a press that becomes
 * a hold gets no `.released` (button_fsm.h). */
static unsigned
handle_running_input(sand_ui_t* ui, const input_t* input) {
    if (input->boot.released) {
        return open_panel(ui, SAND_UI_PALETTE, input->down, SAND_UI_OPEN_PALETTE);
    }

    if (input->power.pressed) {
        return open_panel(ui, SAND_UI_BRUSH, input->down, SAND_UI_OPEN_BRUSH);
    }

    return 0;
}

/* One frame's worth of input, dispatched by screen, see this function's
 * own comment in sand_ui.h. Reading `ui->screen` exactly once, before any
 * branch can change it, is what keeps the press that opens a panel from
 * also being read by that panel's own close check in the same call. */
unsigned
sand_ui_step(sand_ui_t* ui, const input_t* input) {
    if (ui->screen == SAND_UI_MENU) {
        return 0;
    }

    /* The palette closes on BOOT's release: closing on the press left its
     * release to land in SAND_UI_RUNNING. PWR has no release edge
     * (buttons.h), so the brush screen closes on `.pressed`. */
    if (ui->screen == SAND_UI_PALETTE) {
        return handle_panel_input(ui, input, input->boot.released, SAND_UI_CLOSE_PALETTE);
    }

    if (ui->screen == SAND_UI_BRUSH) {
        return handle_panel_input(ui, input, input->power.pressed, SAND_UI_CLOSE_BRUSH);
    }

    return handle_running_input(ui, input);
}
