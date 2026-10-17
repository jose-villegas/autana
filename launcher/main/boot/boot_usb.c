/*
 * boot_usb: puts the board back on USB once startup has tuned PSRAM. The
 * bootloader takes it off (bootloader_components/pmic_cold_boot), because
 * that tuning sweeps the PLL the USB PHY runs on.
 */
#include "sdkconfig.h"

#if CONFIG_ESPTOOLPY_FLASHFREQ_120M
#include "esp_private/startup_internal.h"
#include "hal/usb_serial_jtag_ll.h"

ESP_SYSTEM_INIT_FN(boot_usb_attach, CORE, BIT(0), 0) {
    usb_serial_jtag_ll_phy_disable_pull_override();
    return ESP_OK;
}
#endif
