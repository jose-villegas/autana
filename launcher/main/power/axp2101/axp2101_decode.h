/* axp2101_decode: the AXP2101's battery registers as a snapshot; pure, so it links on a host. */
#pragma once
#include "power/power.h"

typedef struct {
    uint8_t status;
    uint8_t charge;
    uint8_t vbat_high;
    uint8_t vbat_low;
    uint8_t percent;
} axp2101_registers_t;

/* A valid snapshot without read_us, which the caller stamps. */
power_battery_t axp2101_decode(const axp2101_registers_t* regs);
