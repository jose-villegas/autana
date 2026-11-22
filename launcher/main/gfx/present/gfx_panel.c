#include "gfx/present/gfx_panel.h"
#if defined(ESP_PLATFORM) || defined(PANEL_CAPTURE_HOST_TEST)
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
    if (err == 0 && gfx_panel_capture_on) {
        gfx_panel_capture_window(x0, y0, x1, y1, rgb565be);
    }
#endif
    return err;
}
#endif
