#pragma once
#include <stdint.h>

/* The one ROM GPIO call pmic_cold_boot.c makes: route a pad to plain GPIO
 * before driving it. Defined by the bus model (pmic_i2c_bus.c) so the host
 * test can count it. */
void esp_rom_gpio_pad_select_gpio(uint32_t iopad_num);
