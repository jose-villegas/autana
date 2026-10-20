/* A toggle: writes through its pointer and returns true when it changed it. */
#include "ui/ui.h"

typedef struct {
    bool muted;
} toggle_example_t;

bool
toggle_example(toggle_example_t* s) {
    ui_row(UI_ROW_HEIGHT);
    return ui_toggle(ui_next(), "MUTE", &s->muted);
}
