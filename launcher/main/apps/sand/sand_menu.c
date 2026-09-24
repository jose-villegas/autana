#include "sand_menu.h"

void
sand_menu_init(sand_menu_t* menu) {
    menu->screen = SAND_MENU_TITLE;
}

sand_menu_action_t
sand_menu_title_clicked(sand_menu_t* menu, sand_title_button_t button, sand_options_t committed) {
    switch (button) {
        case SAND_TITLE_START: return SAND_MENU_START;
        case SAND_TITLE_EXIT: return SAND_MENU_EXIT;
        case SAND_TITLE_OPTIONS:
            menu->draft = committed;
            menu->screen = SAND_MENU_OPTIONS;
            return SAND_MENU_STAY;
        default: return SAND_MENU_STAY;
    }
}

bool
sand_menu_dither_applies(sand_colour_mode_t color) {
    return color == SAND_COLOUR_16;
}

int
sand_menu_pending_changes(const sand_menu_t* menu, sand_options_t committed) {
    const sand_options_t* d = &menu->draft;
    int pending = (d->quality != committed.quality) + (d->color != committed.color);
    if (sand_menu_dither_applies(d->color)) {
        pending += (d->dither != committed.dither);
    }
    return pending;
}

bool
sand_menu_options_step(sand_menu_t* menu, sand_options_t committed, sand_options_hits_t hits) {
    if (hits.quality >= 0) {
        menu->draft.quality = hits.quality;
    }
    if (hits.color >= 0) {
        menu->draft.color = (sand_colour_mode_t)hits.color;
    }
    if (hits.dither >= 0) {
        menu->draft.dither = hits.dither;
    }
    if (hits.cancel) {
        menu->screen = SAND_MENU_TITLE;
        return false;
    }
    if (!hits.apply || sand_menu_pending_changes(menu, committed) == 0) {
        return false;
    }
    if (!sand_menu_dither_applies(menu->draft.color)) {
        menu->draft.dither = committed.dither;
    }
    menu->screen = SAND_MENU_TITLE;
    return true;
}
