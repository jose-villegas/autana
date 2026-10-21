/* Pure wire reply formatting for battery snapshots. */
#pragma once
#include <ctype.h>
#include <stdio.h>
#include <string.h>
#include "power/power_decode.h"
#include "power/power_log.h"
#define CONSOLE_BATTERY_REPLY_MAX 192

static inline void
console_battery_reply(char* line, size_t size, const power_battery_t* battery, const power_registers_t* regs) {
    (void)snprintf(line, size,
                   "BATTERY valid=%d usb=%d battery=%d mv=%u percent=%u charge=%s flow=%s reg00=0x%02x reg01=0x%02x",
                   battery->valid, battery->usb_present, battery->battery_present, (unsigned)battery->battery_mv,
                   (unsigned)battery->percent, power_charge_name(battery->charge), power_flow_name(battery->flow),
                   regs->status, regs->charge);
}

typedef enum {
    CONSOLE_BATTERY_NOW,
    CONSOLE_BATTERY_LOG,
    CONSOLE_BATTERY_CLEAR,
    CONSOLE_BATTERY_INVALID
} console_battery_action_t;

static inline console_battery_action_t
console_battery_parse(const char* args) {
    char command[sizeof "LOG CLEAR"];
    size_t count = 0;
    while (isspace((unsigned char)*args)) {
        args++;
    }
    while (*args) {
        if (isspace((unsigned char)*args)) {
            while (isspace((unsigned char)*args)) {
                args++;
            }
            if (!*args) {
                break;
            }
            if (count >= sizeof command - 1) {
                return CONSOLE_BATTERY_INVALID;
            }
            command[count++] = ' ';
        } else {
            if (count >= sizeof command - 1) {
                return CONSOLE_BATTERY_INVALID;
            }
            command[count++] = (char)toupper((unsigned char)*args++);
        }
    }
    if (*args) {
        return CONSOLE_BATTERY_INVALID;
    }
    while (count && isspace((unsigned char)command[count - 1])) {
        count--;
    }
    command[count] = '\0';
    if (count == 0) {
        return CONSOLE_BATTERY_NOW;
    }
    if (strcmp(command, "LOG") == 0) {
        return CONSOLE_BATTERY_LOG;
    }
    if (strcmp(command, "LOG CLEAR") == 0) {
        return CONSOLE_BATTERY_CLEAR;
    }
    return CONSOLE_BATTERY_INVALID;
}

static inline void
console_battery_log_reply(char* line, size_t size, const power_log_record_t* record) {
    const power_battery_t* battery = &record->battery;
    (void)snprintf(
        line, size, "BATLOG seq=%lu up_min=%u mv=%u percent=%u usb=%d battery=%d charge=%s flow=%s reason=%s",
        (unsigned long)record->seq, (unsigned)record->up_min, (unsigned)battery->battery_mv, (unsigned)battery->percent,
        battery->usb_present, battery->battery_present, power_charge_name(battery->charge),
        power_flow_name(battery->flow), power_log_reason_name(record->reason));
}
