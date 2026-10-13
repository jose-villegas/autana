/*
 * boot_report: how the last run ended, printed once at the end of boot for a
 * host to read: RESET_REASON=<name> from esp_reset_reason(), and COREDUMP=
 * when the image keeps a core dump in flash. On the board every warm reset
 * becomes a PMIC power cycle (bootloader_components/pmic_cold_boot/), so the
 * reason there reads POWERON after a crash and the core dump is the evidence;
 * docs/notes/Debugging.md says how to read one.
 */
#pragma once

void boot_report_last_reset(void);
