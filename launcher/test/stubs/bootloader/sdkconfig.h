#pragma once

/* Only the flags pmic_cold_boot.c reads: the 120 MHz flash/PSRAM config that
 * turns its bootloader hook on. */
#define CONFIG_ESPTOOLPY_FLASHFREQ_120M  1

/* A development-type build, where a warm reset waits for the host to read. */
#define CONFIG_PMIC_COLD_BOOT_HOST_DRAIN 1
