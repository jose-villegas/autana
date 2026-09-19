#pragma once

#include <stdbool.h>

#include "boot/post.h"

/* What one screenful of the retained self-test results says. `quarter` is
 * the quarter turn the panel is read at, numbered as display.h does;
 * `footer` is optional. With `failures_only` the passing checks and the
 * absent optional peripherals are left out, which is what the boot failure
 * screen wants. */
typedef struct {
    const char* title;
    const char* footer;
    int quarter;
    bool failures_only;
} post_ui_report_t;

/* Draws that screen: a centred title, a summary line, the checks in as many
 * columns as the orientation affords, and the footer along the bottom.
 * Clears the panel first. Does not present - the caller decides when to push
 * the frame. */
void post_ui_draw_report(const post_ui_report_t* report);
