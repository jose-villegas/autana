#include "gfx/present/gfx_panel.h"
#include "build/build_variant.h"
#ifdef ESP_PLATFORM
#include "esp_attr.h"
#include "gfx/present/capture/gfx_panel_capture.h"
#include "gfx/present/gfx_debug.h"
#include "gfx/present/gfx_internal.h"

/* Avoids instruction-cache misses on every panel window.
 * IRAM budget: release 32 B, dev 109 B, diag 109 B */
esp_err_t IRAM_ATTR
gfx_panel_send(int x0, int y0, int x1, int y1, const gfx_color_t* rgb565be) {
#if CONFIG_LAUNCHER_DEVELOPMENT
    dev_bytes_sent += (int64_t)(x1 - x0) * (y1 - y0) * sizeof(gfx_color_t);
    if (gfx_panel_capture_on) {
        gfx_panel_capture_window(x0, y0, x1, y1, rgb565be);
    }
#endif
    return esp_lcd_panel_draw_bitmap(panel, x0, y0, x1, y1, rgb565be);
}
#endif
