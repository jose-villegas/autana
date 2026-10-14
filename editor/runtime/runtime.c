/* Shared layout constraints from firmware headers. */

#include "editor/runtime.h"

#include "gfx/gfx.h"
#include "ui/ui.h"

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
