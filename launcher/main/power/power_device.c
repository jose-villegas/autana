/* The PMU device owner: key events and battery snapshots on the shared bus, through BOARD_PMU_CHIP. */
#include "board/board.h"
#include "core/timing.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "power/power.h"
#include "power/power_cache.h"
#include "power/power_chip.h"
#include "power/power_log.h"

static const char* TAG = "power";
static bool pmu_open;
static bool pmu_ready;
static int64_t last_battery_us;
static portMUX_TYPE lock = portMUX_INITIALIZER_UNLOCKED;

static void
cache_enter(void* context) {
    (void)context;
    portENTER_CRITICAL(&lock);
}

static void
cache_leave(void* context) {
    (void)context;
    portEXIT_CRITICAL(&lock);
}

static power_cache_t cache = {.enter = cache_enter, .leave = cache_leave};

/* POST and buttons_start run before the console task starts. */
static bool
ensure_device(void) {
    if (pmu_open) {
        return true;
    }
    i2c_master_bus_handle_t bus = board_i2c_bus();
    if (bus == NULL) {
        ESP_LOGW(TAG, "No I2C bus - the PWR button will not report");
        return false;
    }
    if (!BOARD_PMU_CHIP.open(bus, BOARD_PMU_I2C_ADDR)) {
        ESP_LOGW(TAG, "Could not add the %s to the bus", BOARD_PMU_CHIP.name);
        return false;
    }
    pmu_open = true;
    return true;
}

void
power_start(void) {
    power_log_warning_t warning;
    if (pmu_ready || !ensure_device() || !BOARD_PMU_CHIP.start(&warning)) {
        return;
    }
    pmu_ready = true;
    ESP_LOGI(TAG, "PWR button via %s, BOOT button on GPIO %d", BOARD_PMU_CHIP.name, BOARD_BOOT_GPIO);

    power_battery_t first;
    (void)power_read_battery_now(&first, NULL);
    last_battery_us = timing_now_us();
    power_log_start(&first, last_battery_us, warning);
}

bool
power_read_battery_now(power_battery_t* out, power_raw_t* raw) {
    const int64_t read_us = timing_now_us();
    power_raw_t local;
    if (raw == NULL) {
        raw = &local;
    }
    *raw = (power_raw_t){0};
    if (!ensure_device() || !BOARD_PMU_CHIP.read_battery(out, raw)) {
        *out = (power_battery_t){0};
    }
    out->read_us = read_us;
    power_cache_store(&cache, out);
    return out->valid;
}

power_battery_t
power_read_battery(void) {
    return power_cache_load(&cache);
}

power_key_events_t
power_poll(void) {
    power_key_events_t events = {0};
    if (pmu_ready) {
        (void)BOARD_PMU_CHIP.take_keys(&events);
    }
    const int64_t now_us = timing_now_us();
    if (pmu_open && ((now_us - last_battery_us) >= POWER_BATTERY_PERIOD_US)) {
        power_battery_t battery;
        (void)power_read_battery_now(&battery, NULL);
        last_battery_us = now_us;
        power_log_poll(&battery, now_us);
    }
    return events;
}
