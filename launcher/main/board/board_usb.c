/*
 * board_usb: keeps the board off USB while startup tunes PSRAM at 120 MHz.
 *
 * That tuning sweeps the PLL which also clocks the USB PHY. A host talking to
 * the USB Serial/JTAG port during the sweep is left with a link that still
 * takes RTS resets but carries no data: the console is silent and esptool
 * cannot sync until the port is replugged. With the D+ pull-up off for the
 * sweep, the host sees a short unplug instead and enumerates the port afresh.
 * Only the PSRAM tuning is wrapped: this board's flash is QIO, whose tuning
 * does not sweep; an octal DTR flash at 120 MHz would sweep there too.
 *
 * Built only with CONFIG_SPIRAM_SPEED_120M, which also adds
 * --wrap=mspi_timing_psram_tuning (main/CMakeLists.txt), so ESP-IDF's PSRAM
 * driver calls __wrap_mspi_timing_psram_tuning in its place.
 * tools/build/check_psram_tuning_wrap.py fails a link that does not.
 */
#include <stdbool.h>
#include <stdint.h>

#include "esp_attr.h"
#include "esp_private/mspi_timing_tuning.h"
#include "hal/usb_serial_jtag_ll.h"

/* The ROM prints to USB Serial/JTAG only while this is set; nothing may
 * print there while the port is off the bus. */
extern bool g_usb_print;

void __real_mspi_timing_psram_tuning(void);
void __wrap_mspi_timing_psram_tuning(void);

_Static_assert(__builtin_types_compatible_p(__typeof__(&mspi_timing_psram_tuning),
                                            __typeof__(&__real_mspi_timing_psram_tuning)),
               "mspi_timing_psram_tuning changed its signature; the wrap must follow it");

/* IRAM, because the wrapped tuning changes the MSPI timing under the cache.
 * The tuning's own "Enter psram timing tuning" line is muted with the rest.
 * IRAM budget: 130 B */
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
}
