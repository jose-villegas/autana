/*
 * board_config: the Waveshare ESP32-S3-Touch-AMOLED-1.8's values for the
 * roles board.h names; board.c asserts the ones the vendor BSP also states.
 */
#pragma once

#include "driver/gpio.h"

/* The panel's size in pixels. */
#define BOARD_PANEL_WIDTH          368
#define BOARD_PANEL_HEIGHT         448

/* BOARD_BOOT_GPIO: pulled up, grounded when pressed, so LOW means down. */
#define BOARD_BOOT_GPIO            GPIO_NUM_0
#define BOARD_PMU_I2C_ADDR         0x34
#define BOARD_IMU_I2C_ADDR         0x6B
/* The input/motion/ chip driver this board carries, by its imu_chip_t symbol. */
#define BOARD_IMU_CHIP             imu_chip_qmi8658
/* The power/ chip driver this board carries, by its power_chip_t symbol. */
#define BOARD_PMU_CHIP             power_chip_axp2101
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
