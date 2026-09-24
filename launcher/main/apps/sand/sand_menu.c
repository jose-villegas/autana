#include "sand_menu.h"

void
sand_menu_init(sand_menu_t* menu, sand_options_t current) {
    menu->screen = SAND_MENU_TITLE;
    menu->committed = current;
    menu->draft = current;
}

sand_menu_action_t
sand_menu_title_clicked(sand_menu_t* menu, sand_title_button_t button) {
    switch (button) {
        case SAND_TITLE_START: return SAND_MENU_START;
        case SAND_TITLE_EXIT: return SAND_MENU_EXIT;
        case SAND_TITLE_OPTIONS:
            menu->draft = menu->committed;
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
sand_menu_pending_changes(const sand_menu_t* menu) {
    const sand_options_t* d = &menu->draft;
    const sand_options_t* c = &menu->committed;
    int pending = (d->quality != c->quality) + (d->color != c->color);
    if (sand_menu_dither_applies(d->color)) {
        pending += (d->dither != c->dither);
    }
    return pending;
}

static bool
apply(sand_menu_t* menu) {
    if (sand_menu_pending_changes(menu) == 0) {
        return false;
    }
    if (!sand_menu_dither_applies(menu->draft.color)) {
        menu->draft.dither = menu->committed.dither;
    }
    menu->committed = menu->draft;
    menu->screen = SAND_MENU_TITLE;
    return true;
}

bool
sand_menu_options_step(sand_menu_t* menu, sand_options_hits_t hits) {
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
        menu->draft = menu->committed;
        menu->screen = SAND_MENU_TITLE;
        return false;
    }
    return hits.apply && apply(menu);
}
