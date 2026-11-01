/* imu_reset: the QMI8658 soft reset, and the register poll it shares with imu.c's commands.
 *
 * An ESP32 reset or flash does not power-cycle the sensor, so without a soft reset
 * whatever the previous firmware left in it survives into the next boot. */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "math/scalar/units.h"

/* Lewis He, SensorLib (MIT), SensorQMI8658.hpp reset(): write the reset command,
 * then poll dQY_L until it reads the done value. The rest of the register map is in imu.c. */
#define REG_RESET            0x60
#define REG_RESET_RESULT     0x4D
#define RESET_COMMAND        0xB0
#define RESET_DONE           0x80
#define RESET_DONE_MASK      0xFF
/* QMI8658A datasheet Rev A: the reset finishes within 15 ms. */
#define RESET_MAX_US         (15U * MATH_US_PER_MS)
/* SensorLib issue #46: the chip NACKs while it resets, so the first poll waits this long. */
#define IMU_RESET_SETTLE_US  (2U * MATH_US_PER_MS)
#define IMU_RESET_POLL_US    MATH_US_PER_MS
/* Polling time after the settle: twice the datasheet maximum, so a slow part is not reported missing. */
#define IMU_RESET_TIMEOUT_US (2U * RESET_MAX_US)

typedef bool (*imu_write_fn)(uint8_t reg, uint8_t value);
typedef bool (*imu_read_fn)(uint8_t reg, uint8_t* out, size_t len);
typedef int64_t (*imu_now_fn)(void);
typedef void (*imu_delay_fn)(uint32_t us);

/* Reads `reg` until (value & mask) == want, at least once, until `timeout_us` passes.
 * A failed read counts as not yet: the chip NACKs while it resets. */
static inline bool
imu_poll(imu_read_fn read_regs, uint8_t reg, uint8_t mask, uint8_t want, imu_now_fn now_us, imu_delay_fn delay_us,
         uint32_t timeout_us, uint32_t poll_us) {
    const int64_t deadline = now_us() + timeout_us;
    do {
        uint8_t value;
        if (read_regs(reg, &value, sizeof value) && (value & mask) == want) {
            return true;
        }
        delay_us(poll_us);
    } while (now_us() < deadline);
    return false;
}

/* Soft-resets the sensor and waits until it is done. The write's own result is ignored:
 * the chip may NACK it as it resets; the poll decides. */
static inline bool
imu_reset(imu_write_fn write_reg, imu_read_fn read_regs, imu_now_fn now_us, imu_delay_fn delay_us) {
    (void)write_reg(REG_RESET, RESET_COMMAND);
    delay_us(IMU_RESET_SETTLE_US);
    return imu_poll(read_regs, REG_RESET_RESULT, RESET_DONE_MASK, RESET_DONE, now_us, delay_us, IMU_RESET_TIMEOUT_US,
                    IMU_RESET_POLL_US);
}
