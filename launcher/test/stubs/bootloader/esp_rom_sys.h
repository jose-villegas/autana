#pragma once
#include <stdint.h>

#include "soc/reset_reasons.h"

/* The two ROM calls pmic_cold_boot.c makes outside of GPIO: read the reset
 * reason once, and a busy-wait it uses both for I2C bit timing and its 20 ms
 * post-restart wait. Both are defined by the bus model (pmic_i2c_bus.c),
 * which records every call instead of actually sleeping. */
soc_reset_reason_t esp_rom_get_reset_reason(int cpu_no);
void esp_rom_delay_us(uint32_t us);
