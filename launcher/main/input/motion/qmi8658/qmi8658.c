/*
 * qmi8658: the QST QMI8658A/C accelerometer and gyroscope, as an imu_chip_t.
 *
 * Register map and the initialisation values follow the QST QMI8658A/C
 * datasheet. The configuration chosen here is the common one for motion sensing
 * rather than navigation: a wide accelerometer range so a knock does not clip,
 * and a rotation range wide enough to survive a deliberate shake.
 */

#include "core/timing.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "input/motion/imu_chip.h"
#include "input/motion/qmi8658/qmi8658_fifo.h"
#include "input/motion/qmi8658/qmi8658_reset.h"
#include "math/scalar/mathi.h"

static const char* TAG = "imu";

/* The QMI8658A's I2C rating (Rev A, Table 43). Faster was measured on A308 and
 * rejected: docs/input/Gravity.md, Limits. */
#define QMI8658_I2C_HZ         400000
#define QMI8658_TIMEOUT_MS     100

/* A drain reads the FIFO in chunks of whole samples, a transaction each: the bus lock is taken
 * per transaction, so a touch read waits for at most one chunk, a tenth of its 10 ms poll.
 * The target counts data bytes only; each transaction's addressing adds about 10%. */
#define IMU_BUS_HOLD_TARGET_US 1000
#define I2C_BITS_PER_BYTE      9 /* 8 data bits and the ACK */
#define IMU_FIFO_SAMPLE_DATA_US                                                                                        \
    ((sizeof(imu_sample_t) * I2C_BITS_PER_BYTE * MATH_US_PER_S + QMI8658_I2C_HZ - 1) / QMI8658_I2C_HZ)
#define IMU_FIFO_CHUNK_SAMPLES ((int)(IMU_BUS_HOLD_TARGET_US / IMU_FIFO_SAMPLE_DATA_US))
_Static_assert(IMU_FIFO_CHUNK_SAMPLES >= 1, "a FIFO chunk must hold a sample");
_Static_assert(IMU_FIFO_CHUNK_SAMPLES <= IMU_FIFO_SAMPLES, "a FIFO chunk must fit the FIFO");

/* Registers; the reset pair and its poll are in qmi8658_reset.h. */
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
#define REG_AX_L                0x35

#define WHO_AM_I_VALUE          0x05

/* CTRL1.ADDR_AI enables bursts; BE stays clear for little-endian axes. */
#define CTRL1_ADDR_AUTO_INC     0x40

/* QMI8658A Rev A, Table 22: full-scale fields in bits 6:4; configure() ORs the 6DOF ODR code into bits 3:0. */
#define CTRL2_ACCEL_8G          0x20
#define CTRL3_GYRO_512DPS       0x50
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

/* What the codes above give, which imu.h and imu_sample.h state every chip delivers. */
#define QMI8658_FIFO_SAMPLES    128  /* FIFO_CTRL_SIZE_128 */
#define QMI8658_COUNTS_PER_G    4096 /* CTRL2_ACCEL_8G */
#define QMI8658_COUNTS_PER_DPS  64   /* CTRL3_GYRO_512DPS: +/-512 dps fills int16 */
_Static_assert(QMI8658_FIFO_SAMPLES == IMU_FIFO_SAMPLES, "imu.h's FIFO depth");
_Static_assert(QMI8658_COUNTS_PER_G == IMU_COUNTS_PER_G && QMI8658_COUNTS_PER_DPS == IMU_COUNTS_PER_DPS,
               "imu_sample.h's scales");
_Static_assert(QMI8658_COUNTS_PER_DPS*(int)IMU_GYRO_RANGE_DPS == (1 << 15), "imu.h's gyro range");

static i2c_master_dev_handle_t dev;
static qmi8658_fifo_t fifo;

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
    return imu_poll(read_regs, REG_STATUSINT, STATUSINT_CMD_DONE, done ? STATUSINT_CMD_DONE : 0, timing_now_us,
                    esp_rom_delay_us, CTRL9_TIMEOUT_US, CTRL9_POLL_US);
}

/* Lewis He, SensorLib (MIT), SensorQMI8658.hpp writeCommand/configFIFO/readFromFifo:
 * QST QMI8658A datasheet Rev A (2022), sections 5.10 and 8 require acknowledgement before REG_FIFO_DATA. */
static bool
write_command(uint8_t command) {
    return write_reg(REG_CTRL9, command) && wait_command(true) && write_reg(REG_CTRL9, CTRL9_ACK)
           && wait_command(false);
}

static bool
qmi8658_configure(unsigned odr_code) {
    qmi8658_fifo_reset(&fifo, odr_code);
    return write_reg(REG_CTRL7, 0) && write_reg(REG_CTRL1, CTRL1_ADDR_AUTO_INC)
           && write_reg(REG_CTRL2, (uint8_t)(CTRL2_ACCEL_8G | odr_code))
           && write_reg(REG_CTRL3, (uint8_t)(CTRL3_GYRO_512DPS | odr_code)) && write_command(CTRL9_RST_FIFO)
           && write_reg(REG_FIFO_CTRL, FIFO_CTRL_CONFIG) && write_reg(REG_CTRL7, CTRL7_ENABLE_ACCEL_GYRO);
}

static bool
qmi8658_open(i2c_master_bus_handle_t bus, uint8_t address, unsigned odr_code) {
    if (dev == NULL) {
        const i2c_device_config_t config = {
            .dev_addr_length = I2C_ADDR_BIT_LEN_7,
            .device_address = address,
            .scl_speed_hz = QMI8658_I2C_HZ,
        };
        if (i2c_master_bus_add_device(bus, &config, &dev) != ESP_OK) {
            ESP_LOGE(TAG, "Could not add the QMI8658 to the bus");
            return false;
        }
    }

    /* Identify before configuring. Something else answering at this address would
     * otherwise be handed a stream of writes it never asked for. */
    uint8_t who = 0;
    if (!read_regs(REG_WHO_AM_I, &who, 1)) {
        ESP_LOGE(TAG, "No response from 0x%02x", address);
        return false;
    }
    if (who != WHO_AM_I_VALUE) {
        ESP_LOGE(TAG, "0x%02x answered WHO_AM_I 0x%02x, expected 0x%02x", address, who, WHO_AM_I_VALUE);
        return false;
    }

    if (!qmi8658_reset(write_reg, read_regs, timing_now_us, esp_rom_delay_us) || !qmi8658_configure(odr_code)) {
        ESP_LOGE(TAG, "Reset or configuration failed");
        return false;
    }

    uint8_t revision = 0;
    read_regs(REG_REVISION, &revision, 1);
    const char* model = revision == REVISION_QMI8658C   ? "QMI8658C"
                        : revision == REVISION_QMI8658A ? "QMI8658A"
                                                        : "unknown";
    ESP_LOGI(TAG, "QMI8658 up, revision 0x%02x (%s), +/-8g and +/-512dps", revision, model);
    return true;
}

static bool
qmi8658_read(imu_sample_t* out) {
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
    *overflowed = fifo.gap || (status[1] & (FIFO_STATUS_FULL | FIFO_STATUS_OVERFLOW)) != 0;
    return *count <= IMU_FIFO_SAMPLES && bytes % sizeof(imu_sample_t) == 0;
}

static void
qmi8658_restart(void) {
    (void)(write_command(CTRL9_RST_FIFO) && write_reg(REG_FIFO_CTRL, FIFO_CTRL_CONFIG));
    qmi8658_fifo_reset(&fifo, fifo.odr_code);
}

/* The confirmed read interval excludes both the request handshake and the FIFO resume write. */
static bool
read_fifo_burst(imu_sample_t out[IMU_FIFO_SAMPLES], int count, uint64_t* locked_us) {
    bool ok = write_command(CTRL9_REQ_FIFO);
    const int64_t lock_start_us = timing_now_us();
    for (int read = 0; ok && read < count;) {
        const int chunk = mathi_min(count - read, IMU_FIFO_CHUNK_SAMPLES);
        ok = read_regs(REG_FIFO_DATA, (uint8_t*)(out + read), (size_t)chunk * sizeof *out);
        read += chunk;
    }
    *locked_us = (uint64_t)(timing_now_us() - lock_start_us);
    return ok;
}

static int
qmi8658_drain(imu_sample_t out[IMU_FIFO_SAMPLES], int* lost, bool* overflowed) {
    *overflowed = false;
    int count = 0;
    uint64_t locked_us = 0;
    const bool ok = read_fifo_count(&count, overflowed) && read_fifo_burst(out, count, &locked_us);
    const bool resumed = write_reg(REG_FIFO_CTRL, FIFO_CTRL_CONFIG);
    fifo.gap = !ok || !resumed;
    if (fifo.gap) {
        /* A failed burst may have consumed part of a sample (reads consume, section 8.8):
         * reset and reconfigure, as qmi8658_open() does, so the next count starts on a sample boundary. */
        (void)(write_command(CTRL9_RST_FIFO) && write_reg(REG_FIFO_CTRL, FIFO_CTRL_CONFIG));
    }
    *lost = qmi8658_fifo_loss(&fifo, locked_us, fifo.gap || *overflowed);
    return fifo.gap ? -1 : count;
}

const imu_chip_t imu_chip_qmi8658 = {
    .name = "QMI8658 accel+gyro",
    .open = qmi8658_open,
    .configure = qmi8658_configure,
    .read = qmi8658_read,
    .drain = qmi8658_drain,
    .restart = qmi8658_restart,
};
