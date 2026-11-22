/* gfx_panel_capture: development capture hooks at panel present boundaries. */
#pragma once
#include <stdbool.h>
#include "build/build_variant.h"
#include "gfx/target/gfx_color.h"

#if BUILD_VARIANT_DEVELOPMENT_OR_HOST
extern bool gfx_panel_capture_on;
void gfx_panel_capture_begin_active(void);
void gfx_panel_capture_commit_active(void);
void gfx_panel_capture_window(int x0, int y0, int x1, int y1, const gfx_color_t* pixels);
#endif

static inline void
gfx_panel_capture_begin(void) {
#if CONFIG_LAUNCHER_DEVELOPMENT || defined(PANEL_CAPTURE_HOST_TEST)
    if (gfx_panel_capture_on) {
        gfx_panel_capture_begin_active();
    }
#endif
}

static inline void
gfx_panel_capture_commit(void) {
#if CONFIG_LAUNCHER_DEVELOPMENT || defined(PANEL_CAPTURE_HOST_TEST)
    if (gfx_panel_capture_on) {
        gfx_panel_capture_commit_active();
    }
#endif
}
