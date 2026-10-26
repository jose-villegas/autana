/* A title bar across the top of the canvas, over the screen's rows. */
#include "ui/ui.h"

typedef struct {
    int page;
} header_example_t;

static const char* const TITLES[] = {"SETTINGS", "DISPLAY"};

bool
header_example(header_example_t* s) {
    ui_header((recti_t){0, 0, ui_width(), UI_TITLE_HEIGHT}, TITLES[s->page]);
    return false;
}
