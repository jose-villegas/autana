/* axp2101: the X-Powers AXP2101 PMU's power-key events and battery registers. */
#include "driver/i2c_master.h"
#include "esp_log.h"
#include "power/axp2101/axp2101_decode.h"
#include "power/axp2101/axp2101_regs.h"
#include "power/power_chip.h"

static const char* TAG = "axp2101";
static i2c_master_dev_handle_t pmu;

static bool
pmu_write(uint8_t reg, uint8_t value) {
    const uint8_t buf[] = {reg, value};
    return i2c_master_transmit(pmu, buf, sizeof(buf), AXP2101_TIMEOUT_MS) == ESP_OK;
}

static bool
pmu_read(uint8_t reg, uint8_t* value) {
    return i2c_master_transmit_receive(pmu, &reg, sizeof reg, value, sizeof *value, AXP2101_TIMEOUT_MS) == ESP_OK;
}

static bool
axp2101_open(i2c_master_bus_handle_t bus, uint8_t address) {
    const i2c_device_config_t config = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = address,
        .scl_speed_hz = AXP2101_I2C_HZ,
    };
    return i2c_master_bus_add_device(bus, &config, &pmu) == ESP_OK;
}

static void
log_thresholds(void) {
    /* 0x22/0x27 default from EFUSE/POR: ask this board once, log, never write. */
    uint8_t levels = 0;
    uint8_t pwroff = 0;
    if (!pmu_read(AXP2101_REG_LEVELS, &levels) || !pmu_read(AXP2101_REG_PWROFF, &pwroff)) {
        ESP_LOGW(TAG, "Could not read 0x22/0x27 - PWR thresholds unknown");
        return;
    }
    static const char* const irq_level[] = {"1s", "1.5s", "2s", "2.5s"};
    static const char* const off_level[] = {"4s", "6s", "8s", "10s"};
    static const char* const on_level[] = {"128ms", "512ms", "1s", "2s"};
    const uint8_t irqlevel = (levels >> AXP2101_IRQ_LEVEL_SHIFT) & AXP2101_LEVEL_MASK;
    const uint8_t offlevel = (levels >> AXP2101_OFF_LEVEL_SHIFT) & AXP2101_LEVEL_MASK;
    const uint8_t onlevel = levels & AXP2101_LEVEL_MASK;
    const bool pwroff_en = (pwroff & AXP2101_PWROFF_ENABLE) != 0;
    const bool pwroff_mode = (pwroff & AXP2101_PWROFF_RESTART) != 0;
    ESP_LOGI(TAG,
             "PWR thresholds: irqlevel %s, offlevel %s, onlevel %s, "
             "pwroff_en %d, pwroff_mode %s",
             irq_level[irqlevel], off_level[offlevel], on_level[onlevel], pwroff_en,
             pwroff_mode ? "restart" : "power-off");
}

static bool
axp2101_start(power_log_warning_t* warning) {
    /* Keep charging and battery interrupt enables intact; they belong to the PMU. */
    uint8_t enables = 0;
    if (!pmu_read(AXP2101_REG_INTEN2, &enables)) {
        ESP_LOGW(TAG, "No answer; PWR button unavailable");
        return false;
    }
    if (!pmu_write(AXP2101_REG_INTEN2, enables | AXP2101_PKEY_SHORT | AXP2101_PKEY_LONG)) {
        ESP_LOGW(TAG, "Could not enable the PWR button interrupts");
        return false;
    }

    /* A press latched before boot must not arrive as the first event. */
    (void)pmu_write(AXP2101_REG_INTSTS2, AXP2101_PKEY_SHORT | AXP2101_PKEY_LONG);

    uint8_t gauge = 0;
    uint8_t adc = 0;
    if (pmu_read(AXP2101_REG_GAUGE_ENABLE, &gauge) && pmu_read(AXP2101_REG_ADC_ENABLE, &adc)) {
        ESP_LOGI(TAG, "Gauge enable %d, VBAT ADC enable %d", (gauge & AXP2101_GAUGE_ENABLE) != 0,
                 (adc & AXP2101_ADC_VBAT_ENABLE) != 0);
    } else {
        ESP_LOGW(TAG, "Could not read gauge/ADC enables");
    }
    uint8_t battery_levels = 0;
    *warning = (power_log_warning_t){0};
    if (pmu_read(AXP2101_REG_BATTERY_LEVELS, &battery_levels)) {
        warning->valid = true;
        warning->warning_percent =
            ((battery_levels >> AXP2101_WARNING_SHIFT) & AXP2101_WARNING_MASK) + AXP2101_WARNING_BASE_PERCENT;
        warning->shutdown_percent = (battery_levels >> AXP2101_SHUTDOWN_SHIFT) & AXP2101_SHUTDOWN_MASK;
    } else {
        ESP_LOGW(TAG, "Could not read battery warning thresholds");
    }
    log_thresholds();
    return true;
}

static bool
axp2101_read_battery(power_battery_t* out, power_raw_t* raw) {
    axp2101_registers_t regs = {0};
    const bool ok = pmu_read(AXP2101_REG_STATUS, &regs.status) && pmu_read(AXP2101_REG_CHARGE, &regs.charge)
                    && pmu_read(AXP2101_REG_VBAT_HIGH, &regs.vbat_high)
                    && pmu_read(AXP2101_REG_VBAT_LOW, &regs.vbat_low) && pmu_read(AXP2101_REG_PERCENT, &regs.percent);
    *raw = (power_raw_t){.status = regs.status, .charge = regs.charge};
    if (ok) {
        *out = axp2101_decode(&regs);
    }
    return ok;
}

static void
axp2101_take_keys(power_key_events_t* out) {
    *out = (power_key_events_t){0};
    uint8_t status = 0;
    if (!pmu_read(AXP2101_REG_INTSTS2, &status)) {
        return;
    }
    const uint8_t fired = status & (AXP2101_PKEY_SHORT | AXP2101_PKEY_LONG);
    if (fired != 0) {
        /* Status is write-one-to-clear; unrelated interrupts must stay latched. */
        (void)pmu_write(AXP2101_REG_INTSTS2, fired);
        out->short_press = (fired & AXP2101_PKEY_SHORT) != 0;
        out->long_press = (fired & AXP2101_PKEY_LONG) != 0;
    }
}

const power_chip_t power_chip_axp2101 = {
    .name = "AXP2101",
    .open = axp2101_open,
    .start = axp2101_start,
    .read_battery = axp2101_read_battery,
    .take_keys = axp2101_take_keys,
};
