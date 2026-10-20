/* A dropdown: a tap opens the list, a pick writes the index and returns true. */
#include "ui/ui.h"

typedef struct {
    int output;
} dropdown_example_t;

static const char* const OUTPUTS[] = {"SPEAKER", "HEADPHONES", "BLUETOOTH", "OFF"};

bool
dropdown_example(dropdown_example_t* s) {
    ui_row(UI_ROW_HEIGHT);
    return ui_dropdown(ui_next(), "output", &s->output, OUTPUTS, 4);
}
