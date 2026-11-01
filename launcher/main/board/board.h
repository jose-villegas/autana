/*
 * board: the one place a board fact gets bound to a concrete value.
 *
 * Every other file names a ROLE (the PMU's address, the boot button's GPIO,
 * the panel's column offset) and never a literal number; see board_esp32s3.c
 * for the binding to the Waveshare ESP32-S3-Touch-AMOLED-1.8.
 */
#pragma once

#include <stdbool.h>

#include <stdint.h>

#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "driver/i2s_std.h"
#include "esp_err.h"
#include "esp_lcd_touch.h"

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

/* The one I2C bus every on-board chip shares, opened on first use; NULL
 * when it cannot be. */
i2c_master_bus_handle_t board_i2c_bus(void);

/* Opens this revision's touch controller, configured with the panel's size
 * and its interrupt pin; callers read both back from (*out)->config. */
esp_err_t board_touch_open(esp_lcd_touch_handle_t* out);

/* The codec's I2S controller and the pins of its link. */
i2s_port_t board_audio_i2s_port(void);
i2s_std_gpio_config_t board_audio_i2s_gpio(void);

/* What a mounted SD card reports about itself. */
#define BOARD_SD_NAME_BYTES 8

typedef struct {
    char name[BOARD_SD_NAME_BYTES];
    uint64_t bytes;
} board_sd_info_t;

/* Mounts the card, fills *out and unmounts it again: a presence test, not a
 * hold. ESP_ERR_NOT_FOUND means no card is inserted. */
esp_err_t board_sd_probe(board_sd_info_t* out);

/* Powers the audio amplifier rail. Behind an IO expander pin on one board
 * and a plain GPIO on another; callers never need to know which. */
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

#define BOARD_BOOT_GPIO            GPIO_NUM_0
#define BOARD_PMU_I2C_ADDR         0x34
#define BOARD_IMU_I2C_ADDR         0x6B
#define BOARD_RTC_I2C_ADDR         0x51
#define BOARD_IO_EXPANDER_I2C_ADDR 0x20
/* The ES8311 datasheet address is 8-bit 0x30; I2C requires the 7-bit address. */
#define BOARD_AUDIO_CODEC_I2C_ADDR 0x18
#define BOARD_TOUCH_FT_I2C_ADDR    0x38
#define BOARD_TOUCH_CST_I2C_ADDR   0x15
#define BOARD_TOUCH_FT_NAME        "FT3168"
#define BOARD_PANEL_X_GAP          0x10
#define BOARD_I2C_SDA_GPIO         GPIO_NUM_15
#define BOARD_I2C_SCL_GPIO         GPIO_NUM_14
