/* power_names: the charge and flow states as the console and POST print them. */
#include "power/power.h"

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
