/* GENERATED FILE - do not edit.
 *
 *     python tools/gen/gen_board_config.py main/board/waveshare_amoled_1_8/board.toml main/board/waveshare_amoled_1_8/board_config.h
 *
 * board_config: Waveshare ESP32-S3-Touch-AMOLED-1.8, from board.toml.
 */
#pragma once

#define BOARD_NAME                    "Waveshare ESP32-S3-Touch-AMOLED-1.8"
#define BOARD_PANEL_WIDTH             368
#define BOARD_PANEL_HEIGHT            448
#define BOARD_PANEL_X_GAP             16
#define BOARD_I2C_SDA_GPIO            15
#define BOARD_I2C_SCL_GPIO            14
#define BOARD_BOOT_GPIO               0
#define BOARD_IMU_CHIP                imu_chip_qmi8658
#define BOARD_IMU_I2C_ADDR            107
#define BOARD_IMU_SIZE_X_MM           3.0F
#define BOARD_IMU_SIZE_Y_MM           2.5F
#define BOARD_IMU_SIZE_Z_MM           0.86F
#define BOARD_IMU_CENTRE_X_MM         (-0.53F)
#define BOARD_IMU_CENTRE_Y_MM         0.22F
#define BOARD_IMU_AXIS_X              {0, 1, 0}
#define BOARD_IMU_AXIS_Y              {1, 0, 0}
#define BOARD_IMU_AXIS_Z              {0, 0, (-1)}
#define BOARD_PMU_CHIP                power_chip_axp2101
#define BOARD_PMU_I2C_ADDR            52
#define BOARD_RTC_I2C_ADDR            81
#define BOARD_IO_EXPANDER_I2C_ADDR    32
#define BOARD_AUDIO_CODEC_I2C_ADDR    24
#define BOARD_TOUCH_FT_NAME           "FT3168"
#define BOARD_TOUCH_FT_I2C_ADDR       56
#define BOARD_TOUCH_CST_I2C_ADDR      21
#define BOARD_DEVICE_WIDTH_MM         33.6F
#define BOARD_DEVICE_HEIGHT_MM        41.2F
#define BOARD_DEVICE_CORNER_RADIUS_MM 6.4F
#define BOARD_DEVICE_DEPTH_MM         7.4F
#define BOARD_CASE_WIDTH_MM           37.6F
#define BOARD_CASE_HEIGHT_MM          45.2F
#define BOARD_CASE_DEPTH_MM           15.0F
#define BOARD_CASE_CORNER_RADIUS_MM   8.4F
#define BOARD_CASE_BACK_SIDE_MM       27.6F
#define BOARD_CASE_SCREW_RADIUS_MM    1.8F
#define BOARD_SCREEN_WIDTH_MM         28.7F
#define BOARD_SCREEN_HEIGHT_MM        34.94F
#define BOARD_IMU_UP_X                1
#define BOARD_IMU_UP_Y                0
#define BOARD_IMU_UP_Z                0
#define BOARD_IMU_TO_SCREEN(x, y, z)  {-(y), (x), (z)}
