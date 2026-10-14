/* Host toolkit initialization and shared layout constraints. */

#include "editor/runtime.h"

#include "gfx/gfx.h"
#include "ui/ui.h"

static bool initialized;

bool
editor_runtime_init(void) {
    if (initialized) {
        return true;
    }
    if (!gfx_init()) {
        return false;
    }

    initialized = true;
    return true;
}

int
editor_runtime_panel_width(void) {
    return GFX_WIDTH;
}

int
editor_runtime_panel_height(void) {
    return GFX_HEIGHT;
}

int
editor_runtime_tap_min(void) {
    return UI_TAP_MIN;
}
