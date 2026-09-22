/*
 * board - ESP32-S3 binding (Waveshare ESP32-S3-Touch-AMOLED-1.8).
 *
 * The BSP ships only the display/touch/I2C/SD building blocks and has no
 * board_detect() of its own, so this file does the probe itself: which
 * touch controller answers decides the panel revision.
 */
#include "board/board.h"

#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "driver/temperature_sensor.h"
#include "esp_check.h"
#include "esp_io_expander_tca9554.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char* TAG = "board";

/* Drives the audio power amplifier directly - this board has no IO
 * expander pin for it. */
#define AUDIO_AMP_GPIO GPIO_NUM_46

static board_variant_t detected = BOARD_VARIANT_UNKNOWN;
static bool amp_gpio_ready;

/* Waveshare's original-revision sketches pulse the IO expander's pins 0-2
 * low then high before touching the panel or touch controller, as a reset.
 * Not every revision has the expander fitted, so this is skipped rather
 * than treated as a fault when nothing answers the probe. */
static void
pulse_io_expander_reset(i2c_master_bus_handle_t bus) {
    esp_io_expander_handle_t expander = NULL;
    if (esp_io_expander_new_i2c_tca9554(bus, BOARD_IO_EXPANDER_I2C_ADDR, &expander) != ESP_OK) {
        return;
    }

    const uint32_t pins = IO_EXPANDER_PIN_NUM_0 | IO_EXPANDER_PIN_NUM_1 | IO_EXPANDER_PIN_NUM_2;
    esp_io_expander_set_dir(expander, pins, IO_EXPANDER_OUTPUT);
    esp_io_expander_set_level(expander, pins, 0);
    vTaskDelay(pdMS_TO_TICKS(20));
    esp_io_expander_set_level(expander, pins, 1);
    vTaskDelay(pdMS_TO_TICKS(50));
    esp_io_expander_del(expander);
}

board_variant_t
board_detect(void) {
    /* A second call must not pulse the reset lines again under a panel
     * that is already running. */
    if (detected != BOARD_VARIANT_UNKNOWN) {
        return detected;
    }

    i2c_master_bus_handle_t bus = bsp_i2c_get_handle();
    if (bus == NULL) {
        ESP_LOGE(TAG, "Could not initialise I2C");
        detected = BOARD_VARIANT_UNKNOWN;
        return detected;
    }

    pulse_io_expander_reset(bus);

    /* CST820 probed first: it is the newer (V2) revision. */
    if (i2c_master_probe(bus, BOARD_TOUCH_CST_I2C_ADDR, 100) == ESP_OK) {
        detected = BOARD_VARIANT_CO5300_CST;
    } else if (i2c_master_probe(bus, BOARD_TOUCH_FT_I2C_ADDR, 100) == ESP_OK) {
        detected = BOARD_VARIANT_SH8601_FT;
    } else {
        ESP_LOGE(TAG, "No supported touch controller answered");
        detected = BOARD_VARIANT_UNKNOWN;
    }
    return detected;
}

board_variant_t
board_variant(void) {
    return detected;
}

const char*
board_variant_name(board_variant_t variant) {
    switch (variant) {
        case BOARD_VARIANT_SH8601_FT: return "SH8601 + FT3168 (original)";
        case BOARD_VARIANT_CO5300_CST: return "CO5300 + CST820 (V2)";
        default: return "unknown";
    }
}

esp_err_t
board_audio_amp_enable(bool on) {
    if (!amp_gpio_ready) {
        const gpio_config_t cfg = {
            .pin_bit_mask = 1ULL << AUDIO_AMP_GPIO,
            .mode = GPIO_MODE_OUTPUT,
        };
        ESP_RETURN_ON_ERROR(gpio_config(&cfg), TAG, "amp gpio config");
        amp_gpio_ready = true;
    }
    return gpio_set_level(AUDIO_AMP_GPIO, on ? 1 : 0);
}

board_temp_sensor_status_t
board_temp_sensor_read_celsius(float* out_celsius) {
#if CONFIG_LAUNCHER_QEMU
    /* QEMU has no such sensor, and ESP-IDF's driver waits on it forever. */
    return BOARD_TEMP_SENSOR_READ_FAILED;
#endif
    temperature_sensor_handle_t sensor = NULL;
    temperature_sensor_config_t cfg = TEMPERATURE_SENSOR_CONFIG_DEFAULT(-10, 80);
    if (temperature_sensor_install(&cfg, &sensor) != ESP_OK) {
        return BOARD_TEMP_SENSOR_INSTALL_FAILED;
    }

    const bool ok =
        temperature_sensor_enable(sensor) == ESP_OK && temperature_sensor_get_celsius(sensor, out_celsius) == ESP_OK;

    temperature_sensor_disable(sensor);
    temperature_sensor_uninstall(sensor);
    return ok ? BOARD_TEMP_SENSOR_OK : BOARD_TEMP_SENSOR_READ_FAILED;
}
