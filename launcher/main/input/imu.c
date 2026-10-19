/*
 * imu: QMI8658 driver.
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
#include "core/timing.h"
#include "driver/i2c_master.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
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
#define REG_CTRL9               0x0A
#define REG_FIFO_CTRL           0x14
#define REG_FIFO_SMPL_CNT       0x15
#define REG_FIFO_DATA           0x17
#define REG_STATUSINT           0x2D
#define REG_AX_L                0x35 /* 12 contiguous bytes: accel then gyro */

#define WHO_AM_I_VALUE          0x05

/* CTRL1.ADDR_AI enables bursts; BE stays clear for little-endian axes. */
#define CTRL1_ADDR_AUTO_INC     0x40

/* QMI8658A Rev A, Table 22: range fields and matching 6DOF ODR codes. */
#define CTRL2_ACCEL_8G          (0x20 | IMU_ODR_CODE)
#define CTRL3_GYRO_512DPS       (0x50 | IMU_ODR_CODE)
#define REVISION_QMI8658C       0x79
#define REVISION_QMI8658A       0x7C
#define CTRL9_ACK               0x00
#define CTRL9_RST_FIFO          0x04
#define CTRL9_REQ_FIFO          0x05
#define STATUSINT_CMD_DONE      0x80
#define FIFO_CTRL_SIZE_128      0x0C
#define FIFO_CTRL_STREAM        0x02
#define FIFO_CTRL_CONFIG        (FIFO_CTRL_SIZE_128 | FIFO_CTRL_STREAM)
#define FIFO_STATUS_COUNT_MSB   0x03
#define FIFO_STATUS_OVERFLOW    0x20
#define FIFO_STATUS_FULL        0x80
#define FIFO_COUNT_MSB_SHIFT    8
#define FIFO_WORD_BYTES         2
#define CTRL9_TIMEOUT_US        (100U * MATH_US_PER_MS)
/* Section 5.10.1 specifies no minimum delay: poll once per I2C status transaction. */
#define CTRL9_POLL_US           0U

/* CTRL7: enable the accelerometer and the gyroscope. */
#define CTRL7_ENABLE_ACCEL_GYRO 0x03

static i2c_master_dev_handle_t dev;
static bool ready;
static bool fifo_gap;
static uint32_t loss_remainder_us;
static int pending_lost;

static bool
write_reg(uint8_t reg, uint8_t value) {
    const uint8_t buf[2] = {reg, value};
    return i2c_master_transmit(dev, buf, sizeof(buf), QMI8658_TIMEOUT_MS) == ESP_OK;
}

static bool
read_regs(uint8_t reg, uint8_t* out, size_t len) {
    return i2c_master_transmit_receive(dev, &reg, 1, out, len, QMI8658_TIMEOUT_MS) == ESP_OK;
}

static bool
wait_command(bool done) {
    const int64_t deadline = timing_now_us() + CTRL9_TIMEOUT_US;
    do {
        uint8_t status;
        if (!read_regs(REG_STATUSINT, &status, sizeof status)) {
            return false;
        }
        if (((status & STATUSINT_CMD_DONE) != 0) == done) {
            return true;
        }
        esp_rom_delay_us(CTRL9_POLL_US);
    } while (timing_now_us() < deadline);
    return false;
}

/* Lewis He, SensorLib (MIT), SensorQMI8658.hpp writeCommand/configFIFO/readFromFifo:
 * QST QMI8658A datasheet Rev A (2022), sections 5.10 and 8 require acknowledgement before REG_FIFO_DATA. */
static bool
write_command(uint8_t command) {
    return write_reg(REG_CTRL9, command) && wait_command(true) && write_reg(REG_CTRL9, CTRL9_ACK)
           && wait_command(false);
}

#if CONFIG_LAUNCHER_DEVELOPMENT
static bool injected;
static int64_t injected_drain_us;
static imu_sample_t injected_sample = {.ax = IMU_COUNTS_PER_G};
static portMUX_TYPE injected_lock = portMUX_INITIALIZER_UNLOCKED;

void
imu_inject(const imu_sample_t* sample) {
    portENTER_CRITICAL(&injected_lock);
    if (!injected) {
        injected_drain_us = timing_now_us();
    }
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

static int
read_injected_fifo(imu_sample_t out[IMU_FIFO_SAMPLES], bool* overflowed) {
    portENTER_CRITICAL(&injected_lock);
    if (!injected) {
        portEXIT_CRITICAL(&injected_lock);
        return -1;
    }
    const imu_sample_t sample = injected_sample;
    const int64_t elapsed = (timing_now_us() - injected_drain_us) / IMU_SAMPLE_US;
    injected_drain_us += elapsed * IMU_SAMPLE_US;
    portEXIT_CRITICAL(&injected_lock);
    const int count = elapsed > IMU_FIFO_SAMPLES ? IMU_FIFO_SAMPLES : (int)elapsed;
    *overflowed = elapsed > IMU_FIFO_SAMPLES;
    for (int i = 0; i < count; i++) {
        memcpy((unsigned char*)out + ((size_t)i * sizeof sample), &sample, sizeof sample);
    }
    return count;
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
        injected_drain_us = timing_now_us();
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

    const bool configured = write_reg(REG_CTRL7, 0) && write_reg(REG_CTRL1, CTRL1_ADDR_AUTO_INC)
                            && write_reg(REG_CTRL2, CTRL2_ACCEL_8G) && write_reg(REG_CTRL3, CTRL3_GYRO_512DPS)
                            && write_command(CTRL9_RST_FIFO) && write_reg(REG_FIFO_CTRL, FIFO_CTRL_CONFIG)
                            && write_reg(REG_CTRL7, CTRL7_ENABLE_ACCEL_GYRO);

    if (!configured) {
        ESP_LOGE(TAG, "Configuration write failed");
        return false;
    }

    uint8_t revision = 0;
    read_regs(REG_REVISION, &revision, 1);
    const char* model = revision == REVISION_QMI8658C   ? "QMI8658C"
                        : revision == REVISION_QMI8658A ? "QMI8658A"
                                                        : "unknown";
    ESP_LOGI(TAG, "QMI8658 up, revision 0x%02x (%s), +/-8g and +/-512dps", revision, model);

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

    return read_regs(REG_AX_L, (uint8_t*)out, sizeof *out);
}

/* Whole samples waiting, before read mode (section 8.8); false on a bus error or a partial sample. */
static bool
read_fifo_count(int* count, bool* overflowed) {
    uint8_t status[2];
    if (!read_regs(REG_FIFO_SMPL_CNT, status, sizeof status)) {
        return false;
    }
    const int words = ((status[1] & FIFO_STATUS_COUNT_MSB) << FIFO_COUNT_MSB_SHIFT) | status[0];
    const int bytes = words * FIFO_WORD_BYTES;
    *count = bytes / (int)sizeof(imu_sample_t);
    *overflowed = fifo_gap || (status[1] & (FIFO_STATUS_FULL | FIFO_STATUS_OVERFLOW)) != 0;
    return *count <= IMU_FIFO_SAMPLES && bytes % sizeof(imu_sample_t) == 0;
}

int
imu_read_fifo(imu_sample_t out[IMU_FIFO_SAMPLES], int* lost, bool* overflowed) {
    *overflowed = false;
    *lost = 0;
    if (!ready || out == NULL) {
        return -1;
    }
#if CONFIG_LAUNCHER_DEVELOPMENT
    const int injected_count = read_injected_fifo(out, overflowed);
    if (injected_count >= 0) {
        pending_lost = 0;
        loss_remainder_us = 0;
        fifo_gap = true;
        return injected_count;
    }
#endif
    int count = 0;
    bool ok = read_fifo_count(&count, overflowed);
    const int64_t lock_start_us = timing_now_us();
    if (ok) {
        ok = write_command(CTRL9_REQ_FIFO);
    }
    if (ok && count > 0) {
        ok = read_regs(REG_FIFO_DATA, (uint8_t*)out, (size_t)count * sizeof *out);
    }
    const bool resumed = write_reg(REG_FIFO_CTRL, FIFO_CTRL_CONFIG);
    const uint64_t locked_us = (uint64_t)(timing_now_us() - lock_start_us);
    fifo_gap = !ok || !resumed;
    if (fifo_gap) {
        /* A failed burst may have consumed part of a sample (reads consume, section 8.8):
         * reset and reconfigure, as imu_init does, so the next count starts on a sample boundary. */
        (void)(write_command(CTRL9_RST_FIFO) && write_reg(REG_FIFO_CTRL, FIFO_CTRL_CONFIG));
    }
    *lost = fifo_gap || *overflowed ? 0 : pending_lost;
    /* Read-mode losses follow this burst, so gravity advances them with the next batch. */
    pending_lost = imu_fifo_lost(locked_us, &loss_remainder_us, fifo_gap || *overflowed);
    return fifo_gap ? -1 : count;
}
