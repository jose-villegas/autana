/*
 * At 120 MHz PSRAM and flash, a warm reset (esptool's RTS, a watchdog, a
 * panic, a restart) hangs in the app's PSRAM timing tuning; a power-on reset
 * does not. So any reset that is not a power-on is turned into one: AXP2101
 * register 0x10 bit 1 power-cycles the SoC, since on this board DCDC1 is
 * VCC3V3 and PWROK drives CHIP_PU. A missing ACK, or a chip still running
 * after the wait, logs and boots on rather than looping. Dropping to 80 MHz
 * was the alternative, rejected as visibly slower.
 */
#include "sdkconfig.h"

/* Flash and PSRAM move to 120 MHz together, and only the flash clock is in
 * the bootloader's configuration. */
#if CONFIG_ESPTOOLPY_FLASHFREQ_120M

#include <stdbool.h>
#include <stdint.h>

#include "esp_log.h"
#include "esp_rom_gpio.h"
#include "esp_rom_sys.h"
#include "hal/gpio_ll.h"
#include "soc/gpio_struct.h"
#include "soc/reset_reasons.h"

/* The app's copies: BOARD_PMU_I2C_ADDR in main/board/board.h, BSP_I2C_SDA/SCL
 * in the BSP header. Neither reaches a bootloader build. */
#define PMIC_SDA_GPIO     15
#define PMIC_SCL_GPIO     14
#define PMIC_I2C_ADDR     0x34
#define PMIC_RESTART_REG  0x10
#define PMIC_RESTART_BIT  (1u << 1)
#define PMIC_POWEROFF_BIT (1u << 0)
#define I2C_HALF_US       5

static bool
needs_cold_restart(soc_reset_reason_t reason) {
    return reason != RESET_REASON_CHIP_POWER_ON;
}

static void
i2c_release(uint32_t pin) {
    gpio_ll_output_disable(&GPIO, pin);
}

static void
i2c_low(uint32_t pin) {
    gpio_ll_output_enable(&GPIO, pin);
}

static void
i2c_wait(void) {
    esp_rom_delay_us(I2C_HALF_US);
}

static void
i2c_start(void) {
    i2c_release(PMIC_SDA_GPIO);
    i2c_release(PMIC_SCL_GPIO);
    i2c_wait();
    i2c_low(PMIC_SDA_GPIO);
    i2c_wait();
    i2c_low(PMIC_SCL_GPIO);
}

static void
i2c_stop(void) {
    i2c_low(PMIC_SDA_GPIO);
    i2c_wait();
    i2c_release(PMIC_SCL_GPIO);
    i2c_wait();
    i2c_release(PMIC_SDA_GPIO);
    i2c_wait();
}

static bool
i2c_write_byte(uint8_t value) {
    for (int bit = 7; bit >= 0; bit--) {
        if (value & (1u << bit)) {
            i2c_release(PMIC_SDA_GPIO);
        } else {
            i2c_low(PMIC_SDA_GPIO);
        }
        i2c_wait();
        i2c_release(PMIC_SCL_GPIO);
        i2c_wait();
        i2c_low(PMIC_SCL_GPIO);
    }

    i2c_release(PMIC_SDA_GPIO);
    i2c_wait();
    i2c_release(PMIC_SCL_GPIO);
    i2c_wait();
    bool acknowledged = gpio_ll_get_level(&GPIO, PMIC_SDA_GPIO) == 0;
    i2c_low(PMIC_SCL_GPIO);
    return acknowledged;
}

static uint8_t
i2c_read_byte(void) {
    uint8_t value = 0;
    i2c_release(PMIC_SDA_GPIO);
    for (int bit = 0; bit < 8; bit++) {
        i2c_wait();
        i2c_release(PMIC_SCL_GPIO);
        i2c_wait();
        value = (uint8_t)((value << 1) | gpio_ll_get_level(&GPIO, PMIC_SDA_GPIO));
        i2c_low(PMIC_SCL_GPIO);
    }

    i2c_wait();
    i2c_release(PMIC_SCL_GPIO);
    i2c_wait();
    i2c_low(PMIC_SCL_GPIO);
    return value;
}

static bool
pmic_restart(void) {
    i2c_start();
    if (!i2c_write_byte(PMIC_I2C_ADDR << 1) || !i2c_write_byte(PMIC_RESTART_REG)) {
        i2c_stop();
        return false;
    }

    i2c_start();
    if (!i2c_write_byte((PMIC_I2C_ADDR << 1) | 1u)) {
        i2c_stop();
        return false;
    }
    uint8_t control = i2c_read_byte();
    i2c_stop();

    /* A corrupted read - a floating line reads 0xFF - must not carry the
     * soft power-off bit back, which would leave the board off. */
    uint8_t restart = (uint8_t)((control & ~PMIC_POWEROFF_BIT) | PMIC_RESTART_BIT);
    i2c_start();
    bool acknowledged =
        i2c_write_byte(PMIC_I2C_ADDR << 1) && i2c_write_byte(PMIC_RESTART_REG) && i2c_write_byte(restart);
    i2c_stop();
    return acknowledged;
}

/* The bootloader links with -u bootloader_hooks_include; this symbol is what
 * pulls the hooks below into the image. */
void
bootloader_hooks_include(void) {}

void
bootloader_after_init(void) {
    soc_reset_reason_t reason = esp_rom_get_reset_reason(0);
    if (!needs_cold_restart(reason)) {
        return;
    }

    ESP_LOGI("pmic_boot", "Reset reason %d: requesting PMIC cold restart", reason);
    esp_rom_gpio_pad_select_gpio(PMIC_SDA_GPIO);
    esp_rom_gpio_pad_select_gpio(PMIC_SCL_GPIO);
    gpio_ll_matrix_out_default(&GPIO, PMIC_SDA_GPIO);
    gpio_ll_matrix_out_default(&GPIO, PMIC_SCL_GPIO);
    gpio_ll_input_enable(&GPIO, PMIC_SDA_GPIO);
    gpio_ll_input_enable(&GPIO, PMIC_SCL_GPIO);
    gpio_ll_set_level(&GPIO, PMIC_SDA_GPIO, 0);
    gpio_ll_set_level(&GPIO, PMIC_SCL_GPIO, 0);
    i2c_release(PMIC_SDA_GPIO);
    i2c_release(PMIC_SCL_GPIO);

    bool acknowledged = pmic_restart();
    i2c_release(PMIC_SDA_GPIO);
    i2c_release(PMIC_SCL_GPIO);
    if (!acknowledged) {
        ESP_LOGW("pmic_boot", "PMIC I2C transfer failed; continuing boot");
        return;
    }

    esp_rom_delay_us(20000);
    ESP_LOGW("pmic_boot", "PMIC restart did not occur; continuing boot");
}

#endif
