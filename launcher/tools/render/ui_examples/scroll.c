/* Flowed rows scroll when their window overflows. */
#include "ui/ui.h"

typedef struct {
    int chosen;
} scroll_example_t;

static const char* const labels[] = {"ONE",   "TWO",   "THREE", "FOUR", "FIVE",   "SIX",
                                     "SEVEN", "EIGHT", "NINE",  "TEN",  "ELEVEN", "TWELVE"};
#define SCROLL_COUNT (sizeof labels / sizeof labels[0])

bool
scroll_example(scroll_example_t* s) {
    bool changed = false;
    ui_scroll_window((recti_t){0, 0, ui_width(), ui_height()});
    for (size_t i = 0; i < SCROLL_COUNT; i++) {
        ui_row(UI_ROW_HEIGHT);
        if (ui_button(ui_next(), labels[i])) {
            s->chosen = (int)i;
            changed = true;
        }
    }
    return changed;
}
