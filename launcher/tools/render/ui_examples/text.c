/* Text in a row: a caption at the left, a value at the right, rebuilt each frame. */
#include <stdio.h>

#include "ui/ui.h"

typedef struct {
    int unread;
} text_example_t;

bool
text_example(text_example_t* s) {
    char count[12];
    snprintf(count, sizeof count, "%d", s->unread);
    ui_row(ui_text_height() + UI_TEXT_PAD);
    const recti_t r = ui_next();
    ui_caption(r, "UNREAD");
    ui_text_right(r, count);
    return false;
}
