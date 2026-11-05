/* imu_chip: what imu.c asks of an accelerometer and gyroscope chip.
 *
 * A chip driver fills one const table, in a folder beside this header; the board's
 * board_config.h names the table its board carries as BOARD_IMU_CHIP. Every driver
 * delivers imu_sample_t at imu.h's scale and FIFO depth, and `rate` is an ODR code
 * as imu.h names them. */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "board/board.h"
#include "driver/i2c_master.h"
#include "input/motion/imu.h"

/* open identifies the chip before configuring it: false if nothing or something else answers.
 * configure and restart start a fresh FIFO; the caller queues its gap and the next drain does not repeat it.
 * drain returns
 * -1 on a bus error and recovers the FIFO itself; *lost counts arrivals dropped before out[0]. */
typedef struct {
    const char* name;
    bool (*open)(i2c_master_bus_handle_t bus, uint8_t address, unsigned rate);
    bool (*configure)(unsigned rate);
    bool (*read)(imu_sample_t* out);
    int (*drain)(imu_sample_t out[IMU_FIFO_SAMPLES], int* lost, bool* overflowed);
    void (*restart)(void);
} imu_chip_t;

/* The chip this board carries. */
extern const imu_chip_t BOARD_IMU_CHIP;
