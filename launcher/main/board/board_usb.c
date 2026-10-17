/*
 * board_usb: keeps the board off USB while startup tunes PSRAM; see
 * board_usb.h.
 */
#include "board/board_usb.h"

#include "sdkconfig.h"

#if CONFIG_SPIRAM_SPEED_120M
#include <stdint.h>

#include "esp_attr.h"
#include "hal/usb_serial_jtag_ll.h"

/* The ROM's switch for its own printing to USB Serial/JTAG. Nothing may be
 * printed there while the port is off the bus. */
extern bool g_usb_print;

void __real_mspi_timing_psram_tuning(void);

static bool detached_for_tuning;

/* main/CMakeLists.txt links with --wrap=mspi_timing_psram_tuning, so ESP-IDF's
 * PSRAM driver calls this in its place. It runs before the cache can be
 * trusted, so it stays in IRAM and touches only registers and ROM data. */
IRAM_ATTR void
__wrap_mspi_timing_psram_tuning(void) {
    const uint32_t conf0 = USB_SERIAL_JTAG.conf0.val;
    const bool usb_print = g_usb_print;
    const usb_serial_jtag_pull_override_vals_t unplugged = {0};

    g_usb_print = false;
    usb_serial_jtag_ll_phy_enable_pull_override(&unplugged);
    __real_mspi_timing_psram_tuning();
    USB_SERIAL_JTAG.conf0.val = conf0;
    g_usb_print = usb_print;
    detached_for_tuning = true;
}

bool
board_usb_detached_for_tuning(void) {
    return detached_for_tuning;
}
#else
bool
board_usb_detached_for_tuning(void) {
    return true;
}
#endif
