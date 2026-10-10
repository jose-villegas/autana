/*
 * boot_report: the RESET_REASON= and COREDUMP= lines boot prints; see
 * boot_report.h.
 */
#include "boot/boot_report.h"

#include <stdio.h>

#include "esp_system.h"
#include "sdkconfig.h"

#if CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH
#include "esp_core_dump.h"
#endif

/* The panic reason a core dump carries, e.g. "assert failed: ..." or an
 * exception name; longer text is cut, the dump itself keeps all of it. */
#define BOOT_REPORT_PANIC_REASON_MAX 120

static const char*
reset_reason_name(esp_reset_reason_t reason) {
    switch (reason) {
        case ESP_RST_POWERON: return "POWERON";
        case ESP_RST_EXT: return "EXT";
        case ESP_RST_SW: return "SW";
        case ESP_RST_PANIC: return "PANIC";
        case ESP_RST_INT_WDT: return "INT_WDT";
        case ESP_RST_TASK_WDT: return "TASK_WDT";
        case ESP_RST_WDT: return "WDT";
        case ESP_RST_DEEPSLEEP: return "DEEPSLEEP";
        case ESP_RST_BROWNOUT: return "BROWNOUT";
        case ESP_RST_SDIO: return "SDIO";
        case ESP_RST_USB: return "USB";
        case ESP_RST_JTAG: return "JTAG";
        case ESP_RST_EFUSE: return "EFUSE";
        case ESP_RST_PWR_GLITCH: return "PWR_GLITCH";
        case ESP_RST_CPU_LOCKUP: return "CPU_LOCKUP";
        case ESP_RST_UNKNOWN:
        default: return "UNKNOWN";
    }
}

#if CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH
static void
report_core_dump(void) {
    if (esp_core_dump_image_check() != ESP_OK) {
        printf("COREDUMP=none\n");
        return;
    }
    char reason[BOOT_REPORT_PANIC_REASON_MAX];
    if (esp_core_dump_get_panic_reason(reason, sizeof reason) != ESP_OK) {
        reason[0] = '\0';
    }
    printf("COREDUMP=present panic=\"%s\"\n", reason);
}
#endif

void
boot_report_last_reset(void) {
    printf("RESET_REASON=%s\n", reset_reason_name(esp_reset_reason()));
#if CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH
    report_core_dump();
#endif
    fflush(stdout);
}
