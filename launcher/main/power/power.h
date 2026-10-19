/* PMU key events and battery snapshots. */
#pragma once
#include <stdbool.h>
#include <stdint.h>

#define POWER_BATTERY_PERIOD_US INT64_C(1000000)

typedef enum {
    POWER_CHARGE_UNKNOWN = 0,
    POWER_CHARGE_TRICKLE,
    POWER_CHARGE_PRE,
    POWER_CHARGE_CC,
    POWER_CHARGE_CV,
    POWER_CHARGE_DONE,
    POWER_CHARGE_IDLE,
} power_charge_t;

typedef enum {
    POWER_FLOW_UNKNOWN = 0,
    POWER_FLOW_STANDBY,
    POWER_FLOW_CHARGING,
    POWER_FLOW_DISCHARGING,
} power_flow_t;

typedef struct {
    bool valid;
    bool usb_present;
    bool battery_present;
    power_charge_t charge;
    power_flow_t flow;
    uint16_t battery_mv;
    uint8_t percent;
    int64_t read_us;
} power_battery_t;

typedef struct {
    bool short_press;
    bool long_press;
} power_key_events_t;

typedef struct {
    uint8_t status;
    uint8_t charge;
    uint8_t vbat_high;
    uint8_t vbat_low;
    uint8_t percent;
} power_registers_t;

void power_start(void);
/* Input polling task only, once per tick (50 Hz): takes and clears PWR-key events (0x49)
 * each call and refreshes the battery once per POWER_BATTERY_PERIOD_US.
 * This is the only steady-state PMU traffic on the shared bus; other bus users
 * may count on that budget. A second task would double both. */
power_key_events_t power_poll(void);
/* Any task: a cached copy, with no I2C. */
power_battery_t power_read_battery(void);
/* POST and the console task only: blocking I2C, then a cache store. */
bool power_read_battery_now(power_battery_t* out);
/* console only: blocking, raw bytes for cross-checks */
bool power_read_battery_raw(power_battery_t* out, power_registers_t* regs);
