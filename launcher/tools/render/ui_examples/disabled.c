/* A disabled scope prevents a disconnected action from firing. */
#include "ui/ui.h"

typedef struct {
    bool connected;
    bool sync_requested;
} disabled_example_t;

bool
disabled_example(disabled_example_t* s) {
    ui_row(UI_ROW_HEIGHT);
    ui_disabled_begin(!s->connected);
    if (ui_button(ui_next(), "SYNC")) {
        s->sync_requested = true;
    }
    ui_disabled_end();
    return s->sync_requested;
}
