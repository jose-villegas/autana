/* Pure wire reply formatting for battery snapshots. */
#pragma once
#include <stdio.h>
#include "power/power_decode.h"
#define CONSOLE_BATTERY_REPLY_MAX 192

static inline void
console_battery_reply(char* line, size_t size, const power_battery_t* battery, const power_registers_t* regs) {
    (void)snprintf(line, size,
                   "BATTERY valid=%d usb=%d battery=%d mv=%u percent=%u charge=%s flow=%s reg00=0x%02x reg01=0x%02x",
                   battery->valid, battery->usb_present, battery->battery_present, (unsigned)battery->battery_mv,
                   (unsigned)battery->percent, power_charge_name(battery->charge), power_flow_name(battery->flow),
                   regs->status, regs->charge);
}
