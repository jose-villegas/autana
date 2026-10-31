/* imu_bringup: the QMI8658 starts every boot from its reset state, then is configured.
 *
 * An ESP32 reset or flash does not power-cycle the sensor, so without a soft reset
 * whatever the previous firmware left in it survives into the next boot. */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "math/scalar/units.h"

/* Lewis He, SensorLib (MIT), SensorQMI8658.hpp reset(): write the reset command,
 * then poll dQY_L until it reads the done value. */
#define QMI8658_REG_RESET        0x60
#define QMI8658_RESET_COMMAND    0xB0
#define QMI8658_REG_RESET_RESULT 0x4D
#define QMI8658_RESET_DONE       0x80
/* QMI8658A datasheet Rev A: the reset finishes within 15 ms. */
#define QMI8658_RESET_MAX_US     (15U * MATH_US_PER_MS)
/* SensorLib issue #46: the chip NACKs while it resets, so the first poll waits this long. */
#define IMU_RESET_SETTLE_US      (2U * MATH_US_PER_MS)
#define IMU_RESET_POLL_US        MATH_US_PER_MS
/* Twice the datasheet maximum, so a slow part is not reported as missing. */
#define IMU_RESET_TIMEOUT_US     (2U * QMI8658_RESET_MAX_US)

/* Soft-resets the sensor, waits for the done value, then runs `configure`.
 * The reset write's own result is ignored: the chip may NACK it as it resets; the poll decides. */
static inline bool
imu_bringup(unsigned odr_code, bool (*write_reg)(uint8_t, uint8_t), bool (*read_regs)(uint8_t, uint8_t*, size_t),
            int64_t (*now_us)(void), void (*delay_us)(uint32_t), bool (*configure)(unsigned)) {
    (void)write_reg(QMI8658_REG_RESET, QMI8658_RESET_COMMAND);
    const int64_t deadline = now_us() + IMU_RESET_TIMEOUT_US;
    delay_us(IMU_RESET_SETTLE_US);
    do {
        uint8_t result;
        if (read_regs(QMI8658_REG_RESET_RESULT, &result, sizeof result) && result == QMI8658_RESET_DONE) {
            return configure(odr_code);
        }
        delay_us(IMU_RESET_POLL_US);
    } while (now_us() < deadline);
    return false;
}
