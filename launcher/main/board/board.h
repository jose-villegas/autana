/*
 * board: the one place a board fact gets bound to a concrete value.
 *
 * Every other file names a ROLE (the PMU's address, the boot button's GPIO,
 * the panel's column offset) and never a literal number. The board Kconfig
 * chooses names a folder under board/ whose board_config.h states the values
 * and whose sources bind these functions. It includes ESP-IDF headers but
 * never the vendor BSP's: only board/'s sources bind to bsp/.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "driver/i2s_std.h"
#include "esp_err.h"
#include "esp_lcd_touch.h"

#include "board_config.h"

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
 * and its interrupt pin; callers read the pin back from (*out)->config. */
esp_err_t board_touch_open(esp_lcd_touch_handle_t* out);

/* The codec's I2S controller and the pins of its link. */
int board_audio_i2s_port(void);
i2s_std_gpio_config_t board_audio_i2s_gpio(void);

/* What a mounted SD card reports about itself. */
#define BOARD_SD_NAME_BYTES 8

typedef struct {
    char name[BOARD_SD_NAME_BYTES];
    uint64_t bytes;
} board_sd_info_t;

/* Mounts the card, fills *out and unmounts it again: a presence test, not a
 * hold. Returns the mount's error when there is no card to read. */
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
