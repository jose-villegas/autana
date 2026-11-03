/* A dropdown whose rows carry an icon beside the name. */
#include "ui/controls/ui_icons.h"
#include "ui/ui.h"

typedef struct {
    int page;
} dropdown_icons_example_t;

static const char* const PAGES[] = {"HOME", "WIFI", "DISPLAY", "GAMES"};

bool
dropdown_icons_example(dropdown_icons_example_t* s) {
    const image_t* const icons[] = {ui_icon_image(UI_ICON_HOME), ui_icon_image(UI_ICON_WIFI),
                                    ui_icon_image(UI_ICON_MONITOR), ui_icon_image(UI_ICON_GAMEPAD)};
    ui_row(UI_ROW_HEIGHT);
    return ui_dropdown_icons(ui_next(), "page", &s->page, PAGES, icons, 4);
}
