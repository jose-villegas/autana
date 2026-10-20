#include "power/power_log.h"

#include <stdio.h>
#include <string.h>

uint16_t
power_log_uptime(int64_t now_us) {
    const uint64_t minutes = now_us > 0 ? (uint64_t)(now_us / POWER_LOG_MINUTE_US) : 0;
    return (uint16_t)(minutes > POWER_LOG_UPTIME_MAX ? POWER_LOG_UPTIME_MAX : minutes);
}

#define POWER_LOG_PACK_FIELD(field, value)  (((uint64_t)(value) & POWER_LOG_##field##_MAX) << POWER_LOG_##field##_SHIFT)
#define POWER_LOG_UNPACK_FIELD(field, type) ((type)((bits >> POWER_LOG_##field##_SHIFT) & POWER_LOG_##field##_MAX))

int64_t
power_log_pack(const power_log_record_t* record) {
    const uint16_t uptime = record->up_min > POWER_LOG_UPTIME_MAX ? POWER_LOG_UPTIME_MAX : record->up_min;
    const uint64_t bits =
        POWER_LOG_PACK_FIELD(SEQ, record->seq) | POWER_LOG_PACK_FIELD(UPTIME, uptime)
        | POWER_LOG_PACK_FIELD(MV, record->battery.battery_mv) | POWER_LOG_PACK_FIELD(PERCENT, record->battery.percent)
        | POWER_LOG_PACK_FIELD(USB, record->battery.usb_present)
        | POWER_LOG_PACK_FIELD(BATTERY, record->battery.battery_present)
        | POWER_LOG_PACK_FIELD(CHARGE, record->battery.charge) | POWER_LOG_PACK_FIELD(FLOW, record->battery.flow)
        | POWER_LOG_PACK_FIELD(REASON, record->reason);
    int64_t packed;
    memcpy(&packed, &bits, sizeof packed);
    return packed;
}

power_log_record_t
power_log_unpack(int64_t packed) {
    uint64_t bits;
    memcpy(&bits, &packed, sizeof bits);
    return (power_log_record_t){.seq = POWER_LOG_UNPACK_FIELD(SEQ, uint32_t),
                                .up_min = POWER_LOG_UNPACK_FIELD(UPTIME, uint16_t),
                                .battery = {.valid = true,
                                            .battery_mv = POWER_LOG_UNPACK_FIELD(MV, uint16_t),
                                            .percent = POWER_LOG_UNPACK_FIELD(PERCENT, uint8_t),
                                            .usb_present = POWER_LOG_UNPACK_FIELD(USB, bool),
                                            .battery_present = POWER_LOG_UNPACK_FIELD(BATTERY, bool),
                                            .charge = POWER_LOG_UNPACK_FIELD(CHARGE, power_charge_t),
                                            .flow = POWER_LOG_UNPACK_FIELD(FLOW, power_flow_t)},
                                .reason = POWER_LOG_UNPACK_FIELD(REASON, power_log_reason_t)};
}

void
power_log_key(char key[POWER_LOG_KEY_SIZE], unsigned slot) {
    (void)snprintf(key, POWER_LOG_KEY_SIZE, POWER_LOG_KEY_FORMAT, slot);
}

int
power_log_newest(const power_log_slot_t slots[POWER_LOG_SLOTS]) {
    int newest = -1;
    for (int i = 0; i < POWER_LOG_SLOTS; i++) {
        if (!slots[i].present) {
            continue;
        }
        const uint32_t distance = newest < 0 ? 0 : (slots[i].seq - slots[newest].seq) & POWER_LOG_SEQ_MAX;
        if (newest < 0 || (distance != 0 && distance < (UINT32_C(1) << (POWER_LOG_SEQ_BITS - 1)))) {
            newest = i;
        }
    }
    return newest;
}

size_t
power_log_order(const power_log_slot_t slots[POWER_LOG_SLOTS], unsigned order[POWER_LOG_SLOTS]) {
    const int newest = power_log_newest(slots);
    size_t count = 0;
    if (newest < 0) {
        return count;
    }
    for (unsigned i = 0; i < POWER_LOG_SLOTS; i++) {
        if (!slots[i].present) {
            continue;
        }
        const uint32_t age = (slots[newest].seq - slots[i].seq) & POWER_LOG_SEQ_MAX;
        size_t at = count;
        while (at > 0 && age > ((slots[newest].seq - slots[order[at - 1]].seq) & POWER_LOG_SEQ_MAX)) {
            order[at] = order[at - 1];
            at--;
        }
        order[at] = i;
        count++;
    }
    return count;
}

power_log_reason_t
power_log_decide(const power_battery_t* previous, const power_battery_t* current, int64_t now_us, int64_t last_us) {
    if (!current->valid) {
        return POWER_LOG_NONE;
    }
    if (previous == NULL) {
        return POWER_LOG_BOOT;
    }
    if (previous->usb_present != current->usb_present || previous->battery_present != current->battery_present
        || previous->charge != current->charge) {
        return POWER_LOG_TRANSITION;
    }
    if (!current->usb_present && now_us >= last_us && now_us - last_us >= POWER_LOG_PERIOD_US) {
        return POWER_LOG_PERIODIC;
    }
    return POWER_LOG_NONE;
}

const char*
power_log_reason_name(power_log_reason_t reason) {
    switch (reason) {
        case POWER_LOG_PERIODIC: return "periodic";
        case POWER_LOG_TRANSITION: return "transition";
        case POWER_LOG_BOOT: return "boot";
        default: return "unknown";
    }
}
