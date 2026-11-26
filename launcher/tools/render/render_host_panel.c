/* The panel and its capture hooks for a render_host program, which has neither:
 * a panel send draws nothing and no capture runs. Strong definitions, since some
 * MinGW linkers leave a call to a weak one unresolved. */
#include <stdbool.h>
#include "gfx/present/capture/gfx_panel_capture.h"
#include "gfx/present/gfx_panel.h"

bool console_panelcap_on;

void
console_panelcap_begin_active(void) {}

void
console_panelcap_commit_active(void) {}

void
console_panelcap_window(int x0, int y0, int x1, int y1, const gfx_color_t* pixels) {
    (void)x0;
    (void)y0;
    (void)x1;
    (void)y1;
    (void)pixels;
}

int
gfx_panel_host_draw(int x0, int y0, int x1, int y1, const gfx_color_t* rgb565be) {
    (void)x0;
    (void)y0;
    (void)x1;
    (void)y1;
    (void)rgb565be;
    return 0;
}
