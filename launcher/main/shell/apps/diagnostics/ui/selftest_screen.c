#include "shell/apps/diagnostics/ui/selftest_screen.h"

#include <stdio.h>

#include "build/build_variant.h"
#include "ui/ui.h"

/* Only a SELFTEST build has a runner to show; a host tests the page. */
#if BUILD_VARIANT_SELFTEST_OR_HOST

#define TEXT_ROW_HEIGHT    (ui_text_height() + UI_TEXT_PAD)
#define HEADING_ROW_HEIGHT (ui_text_height() + (2 * UI_TEXT_PAD))

static void
draw_text_row(int h, const char* s) {
    ui_row(h);
    ui_text(ui_next(), s);
}

/* The button is an ACTION: see app_diagnostics.c's comment on
 * selftest_pending for why selftest_run() never runs from inside it. */
bool
diagnostics_selftest_screen_draw(const diagnostics_selftest_screen_state_t* state) {
    ui_screen();
    ui_row(UI_ROW_HEIGHT);
    const bool clicked = ui_button(ui_next(), "run self test suite");

    char line[48];
    if (state->failures < 0) {
        (void)snprintf(line, sizeof line, "self test: not run yet");
    } else if (state->failures == 0) {
        (void)snprintf(line, sizeof line, "self test: all passed");
    } else {
        (void)snprintf(line, sizeof line, "self test: %d failure(s)", state->failures);
    }
    draw_text_row(TEXT_ROW_HEIGHT, line);

    draw_text_row(HEADING_ROW_HEIGHT, "BOOT for the POST report");
    return clicked;
}

#endif /* BUILD_VARIANT_SELFTEST_OR_HOST */
