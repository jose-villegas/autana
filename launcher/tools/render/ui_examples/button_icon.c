/* A button with an icon at its left: true on the frame a tap is released on it. */
#include "ui/controls/ui_icons.h"
#include "ui/ui.h"

typedef struct {
    bool going_home;
} button_icon_example_t;

bool
button_icon_example(button_icon_example_t* s) {
    ui_row(UI_ROW_HEIGHT);
    const bool tapped = ui_button_icon(ui_next(), "HOME", ui_icon_image(UI_ICON_HOME));
    if (tapped) {
        s->going_home = true;
    }
    return tapped;
}
