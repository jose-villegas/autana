/* Pure battery snapshot decoding. */
#pragma once
#include "power/power.h"

struct power_registers {
    uint8_t status;
    uint8_t charge;
    uint8_t vbat_high;
    uint8_t vbat_low;
    uint8_t percent;
};

power_battery_t power_decode(const power_registers_t* regs, int64_t read_us);
const char* power_charge_name(power_charge_t charge);
const char* power_flow_name(power_flow_t flow);
