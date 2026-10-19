/* Raw snapshots for decoding and console cross-checks. */
#pragma once
#include "power/power.h"

typedef struct {
    uint8_t status;
    uint8_t charge;
    uint8_t vbat_high;
    uint8_t vbat_low;
    uint8_t percent;
} power_registers_t;

power_battery_t power_decode(const power_registers_t* regs, int64_t read_us);
const char* power_charge_name(power_charge_t charge);
const char* power_flow_name(power_flow_t flow);
bool power_read_battery_raw(power_battery_t* out, power_registers_t* regs);
