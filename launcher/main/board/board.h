/*
 * board - the one place a board fact gets bound to a concrete value.
 *
 * Every other file names a ROLE (the PMU's address, the boot button's GPIO,
 * the framebuffer's caps) and never a literal number - see board_esp32s3.c
 * for the binding to the Waveshare ESP32-S3-Touch-AMOLED-1.8.
 */
#pragma once

#include <stdbool.h>

#include "bsp/esp-bsp.h"
#include "esp_err.h"
#include "esp_heap_caps.h"

typedef enum {
    BOARD_VARIANT_UNKNOWN = 0,
    BOARD_VARIANT_SH8601_FT,
    BOARD_VARIANT_CO5300_CST,
} board_variant_t;

/* Initialises I2C, pulses whatever reset lines this board has, and probes
 * the touch controller to tell the two panel/touch revisions apart. Result
 * is cached; board_variant() below reads the cache without probing again. */
board_variant_t board_detect(void);
board_variant_t board_variant(void);
const char* board_variant_name(board_variant_t variant);

/* Powers the audio amplifier rail. Behind an IO expander pin on one board
 * and a plain GPIO on another - callers never need to know which. */
esp_err_t board_audio_amp_enable(bool on);

/* Which step of the on-die temperature sensor's cycle below a failure
 * happened at, so a caller can report "no such sensor" and "sensor found
 * but would not read" as different things rather than both collapsing into
 * one false. */
typedef enum {
    BOARD_TEMP_SENSOR_OK,
    BOARD_TEMP_SENSOR_INSTALL_FAILED,
    BOARD_TEMP_SENSOR_READ_FAILED,
} board_temp_sensor_status_t;

/* One-shot install/enable/read/disable/uninstall of the on-die sensor:
 * fine for an occasional read, not for every frame. Leaves *out_celsius
 * untouched except on BOARD_TEMP_SENSOR_OK. */
board_temp_sensor_status_t board_temp_sensor_read_celsius(float* out_celsius);

/* BOARD_BOOT_GPIO: pulled up, grounded when pressed, so LOW means down. */

#define BOARD_BOOT_GPIO             GPIO_NUM_0
#define BOARD_PMU_I2C_ADDR          0x34
#define BOARD_IMU_I2C_ADDR          0x6B
#define BOARD_RTC_I2C_ADDR          0x51
#define BOARD_IO_EXPANDER_I2C_ADDR  BSP_IO_EXPANDER_I2C_ADDRESS
#define BOARD_TOUCH_FT_I2C_ADDR     0x38
#define BOARD_TOUCH_CST_I2C_ADDR    0x15
#define BOARD_TOUCH_FT_NAME         "FT3168"
#define BOARD_PANEL_X_GAP           0x10

/* The framebuffer does not fit internal SRAM once octal PSRAM is on; gfx
 * copies each strip into internal DMA RAM before it goes to the panel. */
#define BOARD_FRAMEBUFFER_CAPS      (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)
#define BOARD_FRAMEBUFFER_POOL_NAME "SPIRAM"
#define BOARD_I2C_PIN_DESC          "port 0, SDA 15, SCL 14"
