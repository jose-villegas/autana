/* power_chip: what power_device.c asks of a PMU chip.
 *
 * A chip driver fills one const table, in a folder beside this header; the board's
 * board_config.h names the table its board carries as BOARD_PMU_CHIP. */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "board/board.h"
#include "driver/i2c_master.h"
#include "power/power.h"
#include "power/power_log.h"

/* open adds the chip to the bus. start turns the key interrupts on, clears a press latched
 * before boot, logs the chip's settings and fills *warning from its thresholds. read_battery
 * leaves out->read_us to the caller. take_keys reads and clears only the key events. */
typedef struct {
    const char* name;
    bool (*open)(i2c_master_bus_handle_t bus, uint8_t address);
    bool (*start)(power_log_warning_t* warning);
    bool (*read_battery)(power_battery_t* out, power_raw_t* raw);
    bool (*take_keys)(power_key_events_t* out);
} power_chip_t;

/* The chip this board carries. */
extern const power_chip_t BOARD_PMU_CHIP;
