/* A slider over a whole-number range: true on a frame it changed the value. */
#include "ui/ui.h"

typedef struct {
    int volume;
} slider_example_t;

bool
slider_example(slider_example_t* s) {
    ui_row(UI_ROW_HEIGHT);
    return ui_slider(ui_next(), "volume", &s->volume, 0, 10);
}
