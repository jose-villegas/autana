#include "power/power_decode.h"
#include "power/axp2101_regs.h"

power_battery_t
power_decode(const power_registers_t* regs, int64_t read_us) {
    power_battery_t out = {.valid = true,
                           .read_us = read_us,
                           .usb_present = (regs->status & AXP2101_USB_GOOD) != 0,
                           .battery_present = (regs->status & AXP2101_BATTERY_PRESENT) != 0};
    if (!out.battery_present) {
        return out;
    }
    static const power_charge_t charges[] = {POWER_CHARGE_TRICKLE, POWER_CHARGE_PRE,    POWER_CHARGE_CC,
                                             POWER_CHARGE_CV,      POWER_CHARGE_DONE,   POWER_CHARGE_IDLE,
                                             POWER_CHARGE_UNKNOWN, POWER_CHARGE_UNKNOWN};
    static const power_flow_t flows[] = {POWER_FLOW_STANDBY, POWER_FLOW_CHARGING, POWER_FLOW_DISCHARGING,
                                         POWER_FLOW_UNKNOWN};
    out.charge = charges[regs->charge & AXP2101_CHARGE_MASK];
    out.flow = flows[(regs->charge >> AXP2101_FLOW_SHIFT) & AXP2101_FLOW_MASK];
    out.battery_mv =
        (uint16_t)(((regs->vbat_high & AXP2101_VBAT_HIGH_MASK) << AXP2101_VBAT_HIGH_SHIFT) | regs->vbat_low);
    out.percent = (regs->percent <= AXP2101_PERCENT_MAX) ? regs->percent : AXP2101_PERCENT_MAX;
    return out;
}

const char*
power_charge_name(power_charge_t charge) {
    switch (charge) {
        case POWER_CHARGE_TRICKLE: return "trickle";
        case POWER_CHARGE_PRE: return "pre";
        case POWER_CHARGE_CC: return "cc";
        case POWER_CHARGE_CV: return "cv";
        case POWER_CHARGE_DONE: return "done";
        case POWER_CHARGE_IDLE: return "idle";
        default: return "unknown";
    }
}

const char*
power_flow_name(power_flow_t flow) {
    switch (flow) {
        case POWER_FLOW_STANDBY: return "standby";
        case POWER_FLOW_CHARGING: return "charging";
        case POWER_FLOW_DISCHARGING: return "discharging";
        default: return "unknown";
    }
}
