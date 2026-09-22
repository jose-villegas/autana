/*
 * gfx_null_panel - an esp_lcd panel with nothing behind it, for an image
 * that runs where no panel exists (CONFIG_LAUNCHER_QEMU).
 *
 * It keeps the one property of the real link the code above it depends on:
 * a strip occupies the bus for its own transfer time, one strip after
 * another, and only then reports itself sent. Costs therefore keep their
 * order - a narrow window is cheaper than a band, a band cheaper than a
 * frame, nothing sent costs nothing - without any of them being the board's
 * own microseconds.
 */
#pragma once

#include "esp_err.h"
#include "esp_lcd_panel_ops.h"

/* `strip_done` is called once per esp_lcd_panel_draw_bitmap(), from the
 * esp_timer task, when that strip's modelled transfer at `hz` has elapsed.
 * Opening again only changes the clock; the handle is the same one. */
esp_err_t gfx_null_panel_open(int hz, void (*strip_done)(void), esp_lcd_panel_handle_t* out_panel);
