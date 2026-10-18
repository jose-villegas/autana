#include "display/display_host.h"

#include <assert.h>
#include "display/display_shell.h"

static bool held;
static int held_quarter;

void
display_hold_quarter(int quarter) {
    assert(quarter >= DISPLAY_PORTRAIT && quarter <= DISPLAY_LANDSCAPE_UPSIDE_DOWN);
    held_quarter = quarter;
    held = true;
}

void
display_release_quarter(void) {
    held = false;
}

bool
display_quarter_held(void) {
    return held;
}

int
display_host_quarter(int live_quarter) {
    return held ? held_quarter : live_quarter;
}
