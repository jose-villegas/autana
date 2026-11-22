#include "gfx/present/gfx_panel.h"
#include "build/build_variant.h"
#include "gfx/present/capture/gfx_panel_capture.h"
#include "gfx/present/gfx_debug.h"
#include "gfx/present/gfx_internal.h"
#ifdef ESP_PLATFORM
#include "esp_attr.h"
#else
#define IRAM_ATTR
#endif

#if BUILD_VARIANT_DEVELOPMENT_OR_HOST
int64_t dev_bytes_sent;

int64_t
gfx_get_bytes_sent(void) {
    return dev_bytes_sent;
}
#endif

/* Avoids instruction-cache misses on every panel window.
 * IRAM budget: release 32 B, dev 108 B, diag 108 B */
int IRAM_ATTR
gfx_panel_send(int x0, int y0, int x1, int y1, const gfx_color_t* rgb565be) {
#if BUILD_VARIANT_DEVELOPMENT_OR_HOST
    dev_bytes_sent += (int64_t)(x1 - x0) * (y1 - y0) * sizeof(gfx_color_t);
#endif
#ifdef ESP_PLATFORM
    const int err = esp_lcd_panel_draw_bitmap(panel, x0, y0, x1, y1, rgb565be);
#else
    const int err = gfx_panel_host_draw(x0, y0, x1, y1, rgb565be);
#endif
#if BUILD_VARIANT_DEVELOPMENT_OR_HOST
    if (err == 0 && console_panelcap_on) {
        console_panelcap_window(x0, y0, x1, y1, rgb565be);
    }
#endif
    return err;
}

#ifndef ESP_PLATFORM
/* Standalone render tools have no panel or capture sender. */
__attribute__((weak)) bool console_panelcap_on;

__attribute__((weak)) void
console_panelcap_begin_active(void) {}

__attribute__((weak)) void
console_panelcap_commit_active(void) {}

__attribute__((weak)) void
console_panelcap_window(int x0, int y0, int x1, int y1, const gfx_color_t* pixels) {
    (void)x0;
    (void)y0;
    (void)x1;
    (void)y1;
    (void)pixels;
}

__attribute__((weak)) int
gfx_panel_host_draw(int x0, int y0, int x1, int y1, const gfx_color_t* rgb565be) {
    (void)x0;
    (void)y0;
    (void)x1;
    (void)y1;
    (void)rgb565be;
    return 0;
}
#endif
