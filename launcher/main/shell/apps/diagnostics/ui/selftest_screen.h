/* Self-test runner page: the button and the last run's result. */
#pragma once

#include <stdbool.h>

typedef struct {
    int failures; /* -1: never run yet. */
} diagnostics_selftest_screen_state_t;

/* Caller brackets drawing with ui_begin()/ui_end(). Returns true on the frame
 * the run button is tapped. */
bool diagnostics_selftest_screen_draw(const diagnostics_selftest_screen_state_t* state);
