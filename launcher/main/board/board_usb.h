#pragma once
/*
 * board_usb: keeps the board off USB while startup tunes PSRAM.
 *
 * At 120 MHz, ESP-IDF's PSRAM timing tuning sweeps the PLL that also clocks
 * the USB PHY. A host that is talking to the USB Serial/JTAG port during the
 * sweep is left with a link that still takes resets but carries no data: the
 * console is silent and esptool cannot sync. With the D+ pull-up off for the
 * sweep, the host sees a short unplug instead and enumerates the port afresh.
 * Below 120 MHz there is no sweep and nothing to do.
 */
#include <stdbool.h>

/* Whether this boot's PSRAM tuning ran with the port off the bus; true
 * when there was no sweep. ESP-IDF calls the tuning only through the wrap in
 * main/CMakeLists.txt, and false means that call did not go through it. */
bool board_usb_detached_for_tuning(void);
