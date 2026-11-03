#include "power/axp2101/axp2101_decode.h"
#include "power/axp2101/axp2101_regs.h"

power_battery_t
axp2101_decode(const axp2101_registers_t* regs) {
    power_battery_t out = {.valid = true,
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
