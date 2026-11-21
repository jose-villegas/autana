/* gfx_panel: the single dispatch to the physical panel. */
#pragma once

#include "gfx/target/gfx_color.h"
#ifdef ESP_PLATFORM
#include "esp_err.h"
esp_err_t gfx_panel_send(int x0, int y0, int x1, int y1, const gfx_color_t* rgb565be);
#endif
