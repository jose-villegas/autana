/* Host render entry points supply a live quarter without a gravity reader. */
#pragma once

#include <stddef.h>
#include "display/display_shell.h"

static display_orientation_t host_orientation;
static display_quarter_handler_t host_quarter_handler;

void
display_set_quarter_handler(display_quarter_handler_t handler) {
    host_quarter_handler = handler;
}

void
display_hold_quarter(int quarter) {
    const int before = display_quarter_now();
    host_orientation.display.quarter = quarter;
    host_orientation.held = true;
    if (before != quarter && host_quarter_handler != NULL) {
        host_quarter_handler(quarter);
    }
}

void
display_release_quarter(void) {
    host_orientation.held = false;
}

bool
display_quarter_held(void) {
    return host_orientation.held;
}

static int
display_host_quarter(int live_quarter) {
    return host_orientation.held ? host_orientation.display.quarter : live_quarter;
}
