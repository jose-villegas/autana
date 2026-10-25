/*
 * boot_report: how the last run ended, printed once at the end of boot for a
 * host to read: RESET_REASON=<name> from esp_reset_reason(), and COREDUMP=
 * when the image keeps a core dump in flash; docs/notes/Debugging.md says how
 * to read one.
 */
#pragma once

void boot_report_last_reset(void);
