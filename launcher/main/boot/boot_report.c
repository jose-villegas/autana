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

/* The panic reason a core dump carries, e.g. "assert failed: ..." or an
 * exception name; longer text is cut, the dump itself keeps all of it. */
#define BOOT_REPORT_PANIC_REASON_MAX 120
#endif

/* esp_reset_reason_t's values, each printed as its name without ESP_RST_. */
#define RESET_REASONS(X)                                                                                               \
    X(UNKNOWN)                                                                                                         \
    X(POWERON)                                                                                                         \
    X(EXT)                                                                                                             \
    X(SW)                                                                                                              \
    X(PANIC)                                                                                                           \
    X(INT_WDT)                                                                                                         \
    X(TASK_WDT)                                                                                                        \
    X(WDT)                                                                                                             \
    X(DEEPSLEEP)                                                                                                       \
    X(BROWNOUT)                                                                                                        \
    X(SDIO)                                                                                                            \
    X(USB)                                                                                                             \
    X(JTAG)                                                                                                            \
    X(EFUSE)                                                                                                           \
    X(PWR_GLITCH)                                                                                                      \
    X(CPU_LOCKUP)
#define RESET_REASON_NAME(name) [ESP_RST_##name] = #name,

static const char* const reset_reason_names[] = {RESET_REASONS(RESET_REASON_NAME)};

static const char*
reset_reason_name(esp_reset_reason_t reason) {
    const size_t index = (size_t)reason;
    if (index >= sizeof reset_reason_names / sizeof reset_reason_names[0] || reset_reason_names[index] == NULL) {
        return reset_reason_names[ESP_RST_UNKNOWN];
    }
    return reset_reason_names[index];
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
