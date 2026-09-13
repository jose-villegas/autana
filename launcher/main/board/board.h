/*
 * board - the one place a board fact gets bound to a concrete value.
 *
 * Every other file names a ROLE (the PMU's address, the boot button's GPIO,
 * whether PSRAM is expected) and never a target's literal number - see
 * board_esp32c6.c and board_esp32s3.c for the two bindings. Adding a third
 * board means a third board_<target>.c and a third branch below, nothing
 * else in the tree.
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

/* BOARD_BOOT_GPIO on every board: pulled up, grounded when pressed, so LOW
 * means down. */

#if CONFIG_IDF_TARGET_ESP32C6

#define BOARD_BOOT_GPIO             GPIO_NUM_9
#define BOARD_PMU_I2C_ADDR          BSP_PMU_I2C_ADDRESS
#define BOARD_IMU_I2C_ADDR          BSP_IMU_I2C_ADDRESS
#define BOARD_RTC_I2C_ADDR          BSP_RTC_I2C_ADDRESS
#define BOARD_IO_EXPANDER_I2C_ADDR  BSP_IO_EXPANDER_I2C_ADDRESS
#define BOARD_TOUCH_FT_I2C_ADDR     BSP_TOUCH_FT5X06_I2C_ADDRESS
#define BOARD_TOUCH_CST_I2C_ADDR    BSP_TOUCH_CST820_I2C_ADDRESS
#define BOARD_TOUCH_FT_NAME         "FT5x06"

/* The C6's IO expander is load-bearing: it carries the display/touch reset
 * lines, so a board that does not answer here cannot show anything. */
#define BOARD_IO_EXPANDER_REQUIRED  1

/* SD card and display are different pins of the same SPI2 controller. */
#define BOARD_SD_SHARES_DISPLAY_BUS 1

#define BOARD_EXPECTED_PSRAM        0
#define BOARD_PANEL_X_GAP           0 /* unused: this board never brings up a CO5300 panel */
#define BOARD_FRAMEBUFFER_CAPS      (MALLOC_CAP_DMA | MALLOC_CAP_8BIT)
#define BOARD_FRAMEBUFFER_ALIGN     4
#define BOARD_FRAMEBUFFER_POOL_NAME "DMA"
#define BOARD_I2C_PIN_DESC          "port 0, SDA 8, SCL 7"

#elif CONFIG_IDF_TARGET_ESP32S3

#define BOARD_BOOT_GPIO             GPIO_NUM_0
#define BOARD_PMU_I2C_ADDR          0x34
#define BOARD_IMU_I2C_ADDR          0x6B
#define BOARD_RTC_I2C_ADDR          0x51
#define BOARD_IO_EXPANDER_I2C_ADDR  BSP_IO_EXPANDER_I2C_ADDRESS
#define BOARD_TOUCH_FT_I2C_ADDR     0x38
#define BOARD_TOUCH_CST_I2C_ADDR    0x15
#define BOARD_TOUCH_FT_NAME         "FT3168"

/* Not fitted on every revision here - board_detect() pulses it only if it
 * answers, so its absence is not a fault. */
#define BOARD_IO_EXPANDER_REQUIRED  0

/* SD is native 1-bit SDMMC on its own pins, not SPI2. */
#define BOARD_SD_SHARES_DISPLAY_BUS 0

#define BOARD_EXPECTED_PSRAM        1
#define BOARD_PANEL_X_GAP           0x10

/* The framebuffer does not fit internal SRAM once octal PSRAM is on. The
 * panel IO's psram_dma_direct lets the SPI DMA read it in place; without
 * that every strip needs an internal DMA bounce copy, which fails on a
 * fragmented heap and leaves gfx_present() waiting forever. Aligned to the
 * cache line so a full strip (736 bytes per row) needs no copy at all. */
#define BOARD_FRAMEBUFFER_CAPS      (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)
#define BOARD_FRAMEBUFFER_ALIGN     64
#define BOARD_FRAMEBUFFER_POOL_NAME "SPIRAM"
#define BOARD_I2C_PIN_DESC          "port 0, SDA 15, SCL 14"

#else
#error "board.h has no binding for this target - add a board_<target>.c and a branch here"
#endif
