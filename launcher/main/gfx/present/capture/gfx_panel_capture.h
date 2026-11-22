/* gfx_panel_capture: development capture hooks at panel present boundaries. */
#pragma once
#include <stdbool.h>
#include "build/build_variant.h"
#include "gfx/target/gfx_color.h"

#if BUILD_VARIANT_DEVELOPMENT_OR_HOST
extern bool console_panelcap_on;
void console_panelcap_begin_active(void);
void console_panelcap_commit_active(void);
void console_panelcap_window(int x0, int y0, int x1, int y1, const gfx_color_t* pixels);
#endif

static inline void
gfx_panel_capture_begin(void) {
#if BUILD_VARIANT_DEVELOPMENT_OR_HOST
    if (console_panelcap_on) {
        console_panelcap_begin_active();
    }
#endif
}

static inline void
gfx_panel_capture_commit(void) {
#if BUILD_VARIANT_DEVELOPMENT_OR_HOST
    if (console_panelcap_on) {
        console_panelcap_commit_active();
    }
#endif
}
