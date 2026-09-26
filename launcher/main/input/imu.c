/*
 * imu - QMI8658 driver.
 *
 * Register map and the initialisation values follow the QST QMI8658A/C
 * datasheet. The configuration chosen here is the common one for motion sensing
 * rather than navigation: a wide accelerometer range so a knock does not clip,
 * and a rotation range wide enough to survive a deliberate shake.
 */

#include "input/imu.h"

#include <string.h>

#include "board/board.h"
#include "bsp/esp-bsp.h"
#include "driver/i2c_master.h"
#include "esp_log.h"
#if CONFIG_LAUNCHER_DEVELOPMENT
#include "freertos/FreeRTOS.h"
#endif

static const char* TAG = "imu";

#define QMI8658_I2C_HZ          400000
#define QMI8658_TIMEOUT_MS      100

/* Registers */
#define REG_WHO_AM_I            0x00
#define REG_REVISION            0x01
#define REG_CTRL1               0x02
#define REG_CTRL2               0x03
#define REG_CTRL3               0x04
#define REG_CTRL7               0x08
#define REG_AX_L                0x35 /* 12 contiguous bytes: accel then gyro */

#define WHO_AM_I_VALUE          0x05

/* CTRL1: auto-increment the register address on a burst read, which is what
 * makes the single twelve-byte read below legal. */
#define CTRL1_ADDR_AUTO_INC     0x60

/* CTRL2: accelerometer, +/-8 g at ~235 Hz.
 * +/-8 g over a signed 16-bit range is where 4096 counts per g comes from. */
#define CTRL2_ACCEL_8G_235HZ    0x23

/* CTRL3: gyroscope, +/-512 dps at ~235 Hz -> 64 counts per dps. */
#define CTRL3_GYRO_512DPS_235HZ 0x43

/* CTRL7: enable the accelerometer and the gyroscope. */
#define CTRL7_ENABLE_ACCEL_GYRO 0x03

static i2c_master_dev_handle_t dev;
static bool ready;

static bool
write_reg(uint8_t reg, uint8_t value) {
    const uint8_t buf[2] = {reg, value};
    return i2c_master_transmit(dev, buf, sizeof(buf), QMI8658_TIMEOUT_MS) == ESP_OK;
}

static bool
read_regs(uint8_t reg, uint8_t* out, size_t len) {
    return i2c_master_transmit_receive(dev, &reg, 1, out, len, QMI8658_TIMEOUT_MS) == ESP_OK;
}

#if CONFIG_LAUNCHER_DEVELOPMENT
static bool injected;
static imu_sample_t injected_sample = {.ax = IMU_COUNTS_PER_G};
static portMUX_TYPE injected_lock = portMUX_INITIALIZER_UNLOCKED;

void
imu_inject(const imu_sample_t* sample) {
    portENTER_CRITICAL(&injected_lock);
    injected_sample = *sample;
    injected = true;
    portEXIT_CRITICAL(&injected_lock);
}

void
imu_inject_release(void) {
    portENTER_CRITICAL(&injected_lock);
    injected = false;
    portEXIT_CRITICAL(&injected_lock);
}
#endif

bool
imu_init(void) {
    if (ready) {
        return true;
    }

    i2c_master_bus_handle_t bus = bsp_i2c_get_handle();
    if (bus == NULL) {
        ESP_LOGE(TAG, "I2C bus is not initialised");
        return false;
    }

    if (dev == NULL) {
        const i2c_device_config_t config = {
            .dev_addr_length = I2C_ADDR_BIT_LEN_7,
            .device_address = BOARD_IMU_I2C_ADDR,
            .scl_speed_hz = QMI8658_I2C_HZ,
        };
        if (i2c_master_bus_add_device(bus, &config, &dev) != ESP_OK) {
            ESP_LOGE(TAG, "Could not add the QMI8658 to the bus");
            return false;
        }
    }

    /* Identify before configuring. Something else answering at 0x6b would
     * otherwise be handed a stream of writes it never asked for. */
    uint8_t who = 0;
    if (!read_regs(REG_WHO_AM_I, &who, 1)) {
#if CONFIG_LAUNCHER_QEMU
        ESP_LOGW(TAG, "No sensor at 0x%02x; samples come from imu_inject()", BOARD_IMU_I2C_ADDR);
        injected = true;
        ready = true;
        return true;
#endif
        ESP_LOGE(TAG, "No response from 0x%02x", BOARD_IMU_I2C_ADDR);
        return false;
    }
    if (who != WHO_AM_I_VALUE) {
        ESP_LOGE(TAG, "0x%02x answered WHO_AM_I 0x%02x, expected 0x%02x", BOARD_IMU_I2C_ADDR, who, WHO_AM_I_VALUE);
        return false;
    }

    const bool configured = write_reg(REG_CTRL1, CTRL1_ADDR_AUTO_INC) && write_reg(REG_CTRL2, CTRL2_ACCEL_8G_235HZ)
                            && write_reg(REG_CTRL3, CTRL3_GYRO_512DPS_235HZ)
                            && write_reg(REG_CTRL7, CTRL7_ENABLE_ACCEL_GYRO);

    if (!configured) {
        ESP_LOGE(TAG, "Configuration write failed");
        return false;
    }

    uint8_t revision = 0;
    read_regs(REG_REVISION, &revision, 1);
    ESP_LOGI(TAG, "QMI8658 up, revision 0x%02x, +/-8g and +/-512dps", revision);

    ready = true;
    return true;
}

bool
imu_ready(void) {
    return ready;
}

bool
imu_read(imu_sample_t* out) {
    if (!ready) {
        return false;
    }
#if CONFIG_LAUNCHER_DEVELOPMENT
    portENTER_CRITICAL(&injected_lock);
    if (injected) {
        *out = injected_sample;
        portEXIT_CRITICAL(&injected_lock);
        return true;
    }
    portEXIT_CRITICAL(&injected_lock);
#endif

    /* One twelve-byte burst rather than six word reads. Beyond being faster, it
     * guarantees all six axes come from the same sample - reading them
     * separately can straddle an update and produce a vector that never
     * physically existed. */
    uint8_t raw[12];
    if (!read_regs(REG_AX_L, raw, sizeof(raw))) {
        return false;
    }

/* Little-endian pairs. The cast through uint16_t then int16_t is what makes
     * the sign extension well defined. */
#define AXIS(i) ((int16_t)((uint16_t)raw[(i)] | ((uint16_t)raw[(i) + 1] << 8)))

    out->ax = AXIS(0);
    out->ay = AXIS(2);
    out->az = AXIS(4);
    out->gx = AXIS(6);
    out->gy = AXIS(8);
    out->gz = AXIS(10);

    return true;
}
