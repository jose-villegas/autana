/*
 * board_panel: the panel's QSPI link on this board, and which controller's
 * init sequence the detected revision needs (SH8601 or CO5300).
 *
 * gfx owns what is sent and when; this owns how the link is opened.
 */
#pragma once

#include <stddef.h>

#include "esp_err.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"

/* Claims the QSPI bus for transfers up to `max_transfer_bytes`, opens the
 * panel at `hz` and sends the detected revision's init sequence. `on_sent`
 * runs from the interrupt as each transfer lands. */
esp_err_t board_panel_bring_up(int hz, size_t max_transfer_bytes, esp_lcd_panel_io_color_trans_done_cb_t on_sent,
                               esp_lcd_panel_io_handle_t* io, esp_lcd_panel_handle_t* panel);

/* Only the io and driver objects at `hz`. Sends nothing to the panel, which
 * is what lets a clock change reopen them without bringing it up again. */
esp_err_t board_panel_open(int hz, esp_lcd_panel_io_color_trans_done_cb_t on_sent, esp_lcd_panel_io_handle_t* io,
                           esp_lcd_panel_handle_t* panel);
