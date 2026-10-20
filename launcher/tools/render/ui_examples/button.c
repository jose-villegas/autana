/* A button: true on the frame a tap is released on it. */
#include "ui/ui.h"

typedef struct {
    int plays;
} button_example_t;

bool
button_example(button_example_t* s) {
    ui_row(UI_ROW_HEIGHT);
    const bool tapped = ui_button(ui_next(), "PLAY");
    if (tapped) {
        s->plays++;
    }
    return tapped;
}
