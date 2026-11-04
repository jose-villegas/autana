#include "frame/test_frame.h"

#include "gfx/present/gfx_internal.h"

void
test_present_keep_frame(void) {
    if (gfx_mode_current()->layout == GFX_LAYOUT_DOUBLE_FB) {
        (void)gfx_set_clear_on_send(false, 0);
        gfx_present_begin();
    }
}
