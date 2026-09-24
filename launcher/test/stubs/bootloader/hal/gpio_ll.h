#pragma once
#include <stdint.h>

#include "soc/gpio_struct.h"

/* The exact gpio_ll_* surface pmic_cold_boot.c calls. Defined by the bus
 * model (pmic_i2c_bus.c) as real open-drain pin state instead of touching a
 * register, so the same shipped bit-banging drives a simulated AXP2101. */
void gpio_ll_output_enable(gpio_dev_t* hw, uint32_t gpio_num);
void gpio_ll_output_disable(gpio_dev_t* hw, uint32_t gpio_num);
void gpio_ll_input_enable(gpio_dev_t* hw, uint32_t gpio_num);
void gpio_ll_set_level(gpio_dev_t* hw, uint32_t gpio_num, uint32_t level);
int gpio_ll_get_level(gpio_dev_t* hw, uint32_t gpio_num);
void gpio_ll_matrix_out_default(gpio_dev_t* hw, uint32_t gpio_num);
