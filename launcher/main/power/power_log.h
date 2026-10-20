/* power_log: compact battery history and logging policy. */
#pragma once
#include <stddef.h>
#include "power/power.h"
#define POWER_LOG_PERIOD_US     INT64_C(120000000)
#define POWER_LOG_MINUTE_US     INT64_C(60000000)
#define POWER_LOG_SLOTS         360
#define POWER_LOG_SPACE         "power_log"
#define POWER_LOG_KEY_SIZE      5
#define POWER_LOG_KEY_FORMAT    "r%03u"
#define POWER_LOG_MASK(field)   ((UINT64_C(1) << POWER_LOG_##field##_BITS) - 1)
#define POWER_LOG_SEQ_BITS      24
#define POWER_LOG_SEQ_SHIFT     0
#define POWER_LOG_SEQ_MAX       POWER_LOG_MASK(SEQ)
#define POWER_LOG_UPTIME_BITS   10
#define POWER_LOG_UPTIME_SHIFT  24
#define POWER_LOG_UPTIME_MAX    POWER_LOG_MASK(UPTIME)
#define POWER_LOG_MV_BITS       14
#define POWER_LOG_MV_SHIFT      34
#define POWER_LOG_MV_MAX        POWER_LOG_MASK(MV)
#define POWER_LOG_PERCENT_BITS  7
#define POWER_LOG_PERCENT_SHIFT 48
#define POWER_LOG_PERCENT_MAX   POWER_LOG_MASK(PERCENT)
#define POWER_LOG_USB_BITS      1
#define POWER_LOG_USB_SHIFT     55
#define POWER_LOG_USB_MAX       POWER_LOG_MASK(USB)
#define POWER_LOG_BATTERY_BITS  1
#define POWER_LOG_BATTERY_SHIFT 56
#define POWER_LOG_BATTERY_MAX   POWER_LOG_MASK(BATTERY)
#define POWER_LOG_CHARGE_BITS   3
#define POWER_LOG_CHARGE_SHIFT  57
#define POWER_LOG_CHARGE_MAX    POWER_LOG_MASK(CHARGE)
#define POWER_LOG_FLOW_BITS     2
#define POWER_LOG_FLOW_SHIFT    60
#define POWER_LOG_FLOW_MAX      POWER_LOG_MASK(FLOW)
#define POWER_LOG_REASON_BITS   2
#define POWER_LOG_REASON_SHIFT  62
#define POWER_LOG_REASON_MAX    POWER_LOG_MASK(REASON)

typedef enum {
    POWER_LOG_PERIODIC,
    POWER_LOG_TRANSITION,
    POWER_LOG_BOOT,
    POWER_LOG_SHUTDOWN_WARNING,
    POWER_LOG_NONE
} power_log_reason_t;

typedef struct {
    bool valid;
    uint8_t warning_percent;
    uint8_t shutdown_percent;
    bool warning_logged;
    bool shutdown_logged;
} power_log_warning_t;

typedef struct {
    uint32_t seq;
    uint16_t up_min;
    power_battery_t battery;
    power_log_reason_t reason;
} power_log_record_t;

typedef struct {
    bool present;
    uint32_t seq;
} power_log_slot_t;

int64_t power_log_pack(const power_log_record_t* record);
power_log_record_t power_log_unpack(int64_t packed);
uint16_t power_log_uptime(int64_t now_us);
void power_log_key(char key[POWER_LOG_KEY_SIZE], unsigned slot);
int power_log_newest(const power_log_slot_t slots[POWER_LOG_SLOTS]);
size_t power_log_order(const power_log_slot_t slots[POWER_LOG_SLOTS], unsigned order[POWER_LOG_SLOTS]);
power_log_reason_t power_log_decide(const power_battery_t* previous, const power_battery_t* current, int64_t now_us,
                                    int64_t last_us, power_log_warning_t* warning);
const char* power_log_reason_name(power_log_reason_t reason);
void power_log_start(const power_battery_t* battery, int64_t now_us, power_log_warning_t warning);
void power_log_poll(const power_battery_t* battery, int64_t now_us);
bool power_log_clear(void);
int power_log_dump(void (*emit)(const power_log_record_t*, void*), void* context);
