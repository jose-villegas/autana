/* gfx_panel_capture: development capture hooks at panel present boundaries. */
#pragma once
#include <stdbool.h>
#include "build/build_variant.h"
#include "gfx/target/gfx_color.h"

#if defined(ESP_PLATFORM) && CONFIG_LAUNCHER_DEVELOPMENT
extern bool gfx_panel_capture_on;
void gfx_panel_capture_begin(void);
void gfx_panel_capture_commit(void);
void gfx_panel_capture_window(int x0, int y0, int x1, int y1, const gfx_color_t* pixels);
#else
static inline void
gfx_panel_capture_begin(void) {}

static inline void
gfx_panel_capture_commit(void) {}
#endif
