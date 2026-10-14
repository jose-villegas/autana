/* Host toolkit initialization and layout constraints. */
#include "editor/runtime.h"

#include "gfx/gfx.h"
#include "ui/ui.h"

int
main(void) {
    if (!editor_runtime_init() || !editor_runtime_init()) {
        return 1;
    }
    return editor_runtime_panel_width() != GFX_WIDTH || editor_runtime_panel_height() != GFX_HEIGHT
           || editor_runtime_tap_min() != UI_TAP_MIN;
}
