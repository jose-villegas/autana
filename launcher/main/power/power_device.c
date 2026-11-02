/* The PMU device owner: key interrupts and battery snapshots on the shared bus. */
#include "board/board.h"
#include "core/timing.h"
#include "driver/i2c_master.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "power/axp2101_regs.h"
#include "power/power.h"
#include "power/power_cache.h"
#include "power/power_decode.h"
#include "power/power_log.h"

static const char* TAG = "power";
static i2c_master_dev_handle_t pmu;
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

static bool
pmu_write(uint8_t reg, uint8_t value) {
    const uint8_t buf[] = {reg, value};
    return i2c_master_transmit(pmu, buf, sizeof(buf), AXP2101_TIMEOUT_MS) == ESP_OK;
}

static bool
pmu_read(uint8_t reg, uint8_t* value) {
    return i2c_master_transmit_receive(pmu, &reg, sizeof reg, value, sizeof *value, AXP2101_TIMEOUT_MS) == ESP_OK;
}

/* POST and buttons_start run before the console task starts. */
static bool
ensure_device(void) {
    if (pmu != NULL) {
        return true;
    }
    i2c_master_bus_handle_t bus = board_i2c_bus();
    if (bus == NULL) {
        ESP_LOGW(TAG, "No I2C bus - the PWR button will not report");
        return false;
    }

    const i2c_device_config_t config = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = BOARD_PMU_I2C_ADDR,
        .scl_speed_hz = AXP2101_I2C_HZ,
    };
    if (i2c_master_bus_add_device(bus, &config, &pmu) != ESP_OK) {
        ESP_LOGW(TAG, "Could not add the AXP2101 to the bus");
        return false;
    }

    return true;
}

void
power_start(void) {
    if (pmu_ready || !ensure_device()) {
        return;
    }
    /* Keep charging and battery interrupt enables intact; they belong to the PMU. */
    uint8_t enables = 0;
    if (!pmu_read(AXP2101_REG_INTEN2, &enables)) {
        ESP_LOGW(TAG, "AXP2101 did not answer; PWR button unavailable");
        return;
    }
    if (!pmu_write(AXP2101_REG_INTEN2, enables | AXP2101_PKEY_SHORT | AXP2101_PKEY_LONG)) {
        ESP_LOGW(TAG, "Could not enable the PWR button interrupts");
        return;
    }

    /* A press latched before boot must not arrive as the first event. */
    (void)pmu_write(AXP2101_REG_INTSTS2, AXP2101_PKEY_SHORT | AXP2101_PKEY_LONG);

    pmu_ready = true;
    ESP_LOGI(TAG, "PWR button via AXP2101, BOOT button on GPIO %d", BOARD_BOOT_GPIO);

    uint8_t gauge = 0;
    uint8_t adc = 0;
    if (pmu_read(AXP2101_REG_GAUGE_ENABLE, &gauge) && pmu_read(AXP2101_REG_ADC_ENABLE, &adc)) {
        ESP_LOGI(TAG, "AXP2101 gauge enable %d, VBAT ADC enable %d", (gauge & AXP2101_GAUGE_ENABLE) != 0,
                 (adc & AXP2101_ADC_VBAT_ENABLE) != 0);
    } else {
        ESP_LOGW(TAG, "Could not read AXP2101 gauge/ADC enables");
    }
    uint8_t battery_levels = 0;
    power_log_warning_t warning = {0};
    if (pmu_read(AXP2101_REG_BATTERY_LEVELS, &battery_levels)) {
        warning.valid = true;
        warning.warning_percent =
            ((battery_levels >> AXP2101_WARNING_SHIFT) & AXP2101_WARNING_MASK) + AXP2101_WARNING_BASE_PERCENT;
        warning.shutdown_percent = (battery_levels >> AXP2101_SHUTDOWN_SHIFT) & AXP2101_SHUTDOWN_MASK;
    } else {
        ESP_LOGW(TAG, "Could not read AXP2101 battery warning thresholds");
    }
    power_battery_t first;
    (void)power_read_battery_now(&first, NULL);
    last_battery_us = timing_now_us();
    power_log_start(&first, last_battery_us, warning);

    /* 0x22/0x27 default from EFUSE/POR: ask this board once, log, never write. */
    uint8_t levels = 0;
    uint8_t pwroff = 0;
    if (pmu_read(AXP2101_REG_LEVELS, &levels) && pmu_read(AXP2101_REG_PWROFF, &pwroff)) {
        static const char* const irq_level[] = {"1s", "1.5s", "2s", "2.5s"};
        static const char* const off_level[] = {"4s", "6s", "8s", "10s"};
        static const char* const on_level[] = {"128ms", "512ms", "1s", "2s"};
        const uint8_t irqlevel = (levels >> AXP2101_IRQ_LEVEL_SHIFT) & AXP2101_LEVEL_MASK;
        const uint8_t offlevel = (levels >> AXP2101_OFF_LEVEL_SHIFT) & AXP2101_LEVEL_MASK;
        const uint8_t onlevel = levels & AXP2101_LEVEL_MASK;
        const bool pwroff_en = (pwroff & AXP2101_PWROFF_ENABLE) != 0;
        const bool pwroff_mode = (pwroff & AXP2101_PWROFF_RESTART) != 0;
        ESP_LOGI(TAG,
                 "AXP2101 PWR thresholds: irqlevel %s, offlevel %s, onlevel %s, "
                 "pwroff_en %d, pwroff_mode %s",
                 irq_level[irqlevel], off_level[offlevel], on_level[onlevel], pwroff_en,
                 pwroff_mode ? "restart" : "power-off");
    } else {
        ESP_LOGW(TAG, "Could not read AXP2101 0x22/0x27 - PWR thresholds unknown");
    }
}

bool
power_read_battery_now(power_battery_t* out, power_registers_t* regs) {
    const int64_t read_us = timing_now_us();
    power_registers_t local;
    if (regs == NULL) {
        regs = &local;
    }
    *regs = (power_registers_t){0};
    const bool ok = ensure_device() && pmu_read(AXP2101_REG_STATUS, &regs->status)
                    && pmu_read(AXP2101_REG_CHARGE, &regs->charge) && pmu_read(AXP2101_REG_VBAT_HIGH, &regs->vbat_high)
                    && pmu_read(AXP2101_REG_VBAT_LOW, &regs->vbat_low) && pmu_read(AXP2101_REG_PERCENT, &regs->percent);
    *out = ok ? power_decode(regs, read_us) : (power_battery_t){.read_us = read_us};
    power_cache_store(&cache, out);
    return ok;
}

power_battery_t
power_read_battery(void) {
    return power_cache_load(&cache);
}

power_key_events_t
power_poll(void) {
    power_key_events_t events = {0};
    uint8_t status = 0;
    if (pmu_ready && pmu_read(AXP2101_REG_INTSTS2, &status)) {
        const uint8_t fired = status & (AXP2101_PKEY_SHORT | AXP2101_PKEY_LONG);
        if (fired != 0) {
            /* Status is write-one-to-clear; unrelated interrupts must stay latched. */
            (void)pmu_write(AXP2101_REG_INTSTS2, fired);
            events.short_press = (fired & AXP2101_PKEY_SHORT) != 0;
            events.long_press = (fired & AXP2101_PKEY_LONG) != 0;
        }
    }
    const int64_t now_us = timing_now_us();
    if ((pmu != NULL) && ((now_us - last_battery_us) >= POWER_BATTERY_PERIOD_US)) {
        power_battery_t battery;
        (void)power_read_battery_now(&battery, NULL);
        last_battery_us = now_us;
        power_log_poll(&battery, now_us);
    }
    return events;
}
