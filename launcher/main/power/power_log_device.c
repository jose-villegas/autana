/* power_log_device: battery history in the settings store, serialized with console access. */
#include "power/power_log.h"

#include <stdlib.h>
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "services/settings.h"

static StaticSemaphore_t log_mutex_storage;
static SemaphoreHandle_t log_mutex;
static unsigned next_slot;
static uint32_t next_seq;
static power_battery_t previous;
static bool have_previous;
static int64_t last_log_us;
static bool failure_reported;

static void
read_slots(power_log_slot_t slots[POWER_LOG_SLOTS], int64_t* packed) {
    for (unsigned i = 0; i < POWER_LOG_SLOTS; i++) {
        char key[POWER_LOG_KEY_SIZE];
        power_log_key(key, i);
        int64_t value = 0;
        const bool present = settings_read_i64(POWER_LOG_SPACE, key, &value);
        slots[i] = (power_log_slot_t){.present = present, .seq = power_log_unpack(value).seq};
        if (packed != NULL) {
            packed[i] = value;
        }
    }
}

static void
append(const power_battery_t* battery, int64_t now_us, power_log_reason_t reason) {
    const power_log_record_t record = {
        .seq = next_seq, .up_min = power_log_uptime(now_us), .battery = *battery, .reason = reason};
    char key[POWER_LOG_KEY_SIZE];
    power_log_key(key, next_slot);
    if (settings_write_i64(POWER_LOG_SPACE, key, power_log_pack(&record))) {
        next_slot = (next_slot + 1) % POWER_LOG_SLOTS;
        next_seq = (next_seq + 1) & POWER_LOG_SEQ_MAX;
    } else if (!failure_reported) {
        ESP_LOGW("power", "Battery history write failed");
        failure_reported = true;
    }
    previous = *battery;
    have_previous = true;
    last_log_us = now_us;
}

void
power_log_start(const power_battery_t* battery, int64_t now_us) {
    log_mutex = xSemaphoreCreateMutexStatic(&log_mutex_storage);
    power_log_slot_t* slots = calloc(POWER_LOG_SLOTS, sizeof *slots);
    if (slots == NULL) {
        log_mutex = NULL;
        return;
    }
    read_slots(slots, NULL);
    const int newest = power_log_newest(slots);
    if (newest >= 0) {
        next_slot = ((unsigned)newest + 1) % POWER_LOG_SLOTS;
        next_seq = (slots[newest].seq + 1) & POWER_LOG_SEQ_MAX;
    }
    free(slots);
    power_log_poll(battery, now_us);
}

void
power_log_poll(const power_battery_t* battery, int64_t now_us) {
    if (log_mutex == NULL) {
        return;
    }
    xSemaphoreTake(log_mutex, portMAX_DELAY);
    const power_log_reason_t reason = power_log_decide(have_previous ? &previous : NULL, battery, now_us, last_log_us);
    if (reason != POWER_LOG_NONE) {
        append(battery, now_us, reason);
    }
    xSemaphoreGive(log_mutex);
}

bool
power_log_clear(void) {
    if (log_mutex == NULL) {
        return false;
    }
    xSemaphoreTake(log_mutex, portMAX_DELAY);
    const bool ok = settings_erase_space(POWER_LOG_SPACE);
    if (ok) {
        next_slot = 0;
        next_seq = 0;
    }
    xSemaphoreGive(log_mutex);
    return ok;
}

int
power_log_dump(void (*emit)(const power_log_record_t*, void*), void* context) {
    if (log_mutex == NULL) {
        return -1;
    }
    power_log_slot_t* slots = calloc(POWER_LOG_SLOTS, sizeof *slots);
    int64_t* packed = malloc(POWER_LOG_SLOTS * sizeof *packed);
    unsigned* order = malloc(POWER_LOG_SLOTS * sizeof *order);
    const bool ok = slots != NULL && packed != NULL && order != NULL;
    int count = -1;
    if (ok) {
        xSemaphoreTake(log_mutex, portMAX_DELAY);
        read_slots(slots, packed);
        xSemaphoreGive(log_mutex);
        count = (int)power_log_order(slots, order);
        for (int i = 0; i < count; i++) {
            const power_log_record_t record = power_log_unpack(packed[order[i]]);
            emit(&record, context);
        }
    }
    free(order);
    free(packed);
    free(slots);
    return count;
}
