/* gfx_panel: the single dispatch to the physical panel. */
#pragma once

#include "gfx/target/gfx_color.h"

#if defined(ESP_PLATFORM) || defined(PANEL_CAPTURE_HOST_TEST)
int gfx_panel_send(int x0, int y0, int x1, int y1, const gfx_color_t* rgb565be);
#ifndef ESP_PLATFORM
int gfx_panel_host_draw(int x0, int y0, int x1, int y1, const gfx_color_t* rgb565be);
#endif
#endif
