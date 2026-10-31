/*
 * imu: QMI8658 driver.
 *
 * Register map and the initialisation values follow the QST QMI8658A/C
 * datasheet. The configuration chosen here is the common one for motion sensing
 * rather than navigation: a wide accelerometer range so a knock does not clip,
 * and a rotation range wide enough to survive a deliberate shake.
 */

#include "input/motion/imu.h"

#include <string.h>

#include "board/board.h"
#include "bsp/esp-bsp.h"
#include "core/job.h"
#include "core/memory.h"
#include "core/timing.h"
#include "core/tune.h"
#include "driver/i2c_master.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "input/motion/imu_demand.h"
#include "input/motion/imu_odr.h"
#include "input/motion/imu_queue.h"

static const char* TAG = "imu";

/* 50 Hz register reads and 10 Hz FIFO drains: the newest sample averages ~11 ms old,
 * with ~7% read-mode loss. Each I2C transaction costs the simulation cores ~40 us,
 * so the FIFO is drained only at 10 Hz. */
#define IMU_READ_HZ          50
#define IMU_FIFO_DRAIN_HZ    10
#define IMU_FIFO_DRAIN_TICKS (IMU_READ_HZ / IMU_FIFO_DRAIN_HZ)
#define IMU_READ_MS          (MATH_MS_PER_S / IMU_READ_HZ)
#define IMU_DRAIN_MS         (IMU_FIFO_DRAIN_TICKS * IMU_READ_MS)
#define IMU_PERIOD_MIN_MS    1
#define IMU_PERIOD_MAX_MS    ((IMU_FIFO_SAMPLES * IMU_SAMPLE_US - 1) / MATH_US_PER_MS)

_Static_assert(IMU_READ_HZ % IMU_FIFO_DRAIN_HZ == 0, "FIFO drains need whole read periods");
_Static_assert(IMU_DRAIN_MS* MATH_US_PER_MS < IMU_FIFO_SAMPLES * IMU_SAMPLE_US,
               "FIFO drain period must fit retained history");
/* Starts each tick while the core-1 job worker is idle.
 * A drain delayed past IMU_FIFO_SAMPLES * IMU_SAMPLE_US surfaces as a gap (gravity restart). */
#define IMU_TASK_CORE         JOB_CORE1
#define IMU_TASK_PRIORITY     (JOB_CORE1_PRIORITY - 1)
#define IMU_TASK_BUS_PRIORITY (JOB_CORE1_PRIORITY + 1)
_Static_assert(IMU_TASK_PRIORITY > tskIDLE_PRIORITY, "IMU task must run above idle priority");
_Static_assert(IMU_TASK_BUS_PRIORITY < configMAX_PRIORITIES, "IMU bus priority must fit task priorities");
/* Board dev stats: at worst 1440 of 3072 stack bytes free, idle and under load,
 * with no bus error. The bus-error path (driver ESP_LOGE) was not exercised. */
#define IMU_TASK_STACK_BYTES (3 * 1024)

TUNE_OWNER(imu);
TUNE(imu, read_ms, IMU_READ_MS, IMU_PERIOD_MIN_MS, IMU_PERIOD_MAX_MS);
TUNE(imu, drain_ms, IMU_DRAIN_MS, IMU_PERIOD_MIN_MS, IMU_PERIOD_MAX_MS);
#if CONFIG_LAUNCHER_DEVELOPMENT
/* Upper bound for the reporting window. */
#define IMU_LOG_MAX_MS (60 * MATH_MS_PER_S)
TUNE(imu, log_ms, 0, 0, IMU_LOG_MAX_MS);
#endif

#if CONFIG_LAUNCHER_DEVELOPMENT
/* imu_set_odr() asks, the task applies between drains; both under `lock`. */
static imu_odr_t odr = {.requested = IMU_ODR_CODE, .applied = IMU_ODR_CODE};
#endif
static portMUX_TYPE lock = portMUX_INITIALIZER_UNLOCKED;
static imu_queue_t queue;
static imu_sample_t newest;
static uint32_t newest_sequence;
static bool have_newest;
static TaskHandle_t task_handle;
static imu_demand_t demand = {.newest_us = IMU_DEMAND_NEVER, .fifo_us = IMU_DEMAND_NEVER};
/* Set by the task before it waits for a notification; the next reader clears it and notifies. */
static bool parked;
#if CONFIG_LAUNCHER_DEVELOPMENT
static int64_t newest_us;
static uint64_t reader_age_total_us;
static int64_t reader_age_max_us;
static unsigned reader_age_count;

/* One imu.log_ms window of task work; the task alone writes it. */
typedef struct {
    int64_t last_log_us;
    int64_t last_wake_us;
    uint64_t drain_total_us;
    int64_t drain_max_us;
    uint64_t bus_us;
    uint64_t parked_us;
    int64_t wake_gap_max_us;
    unsigned register_reads;
    unsigned drains;
    unsigned delivered;
    unsigned lost;
    unsigned parks;
} imu_stats_t;

static imu_stats_t stats;
#endif

/* The QMI8658A's I2C rating (Rev A, Table 43). Faster was measured on A308 and
 * rejected: docs/input/Gravity.md, Limits. */
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

static i2c_master_dev_handle_t dev;
static bool ready;
static bool sensor_present;
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

/* Sensors off, ranges and rate, a fresh FIFO, sensors on. */
static bool
configure(unsigned odr_code) {
    return write_reg(REG_CTRL7, 0) && write_reg(REG_CTRL1, CTRL1_ADDR_AUTO_INC)
           && write_reg(REG_CTRL2, (uint8_t)(CTRL2_ACCEL_8G | odr_code))
           && write_reg(REG_CTRL3, (uint8_t)(CTRL3_GYRO_512DPS | odr_code)) && write_command(CTRL9_RST_FIFO)
           && write_reg(REG_FIFO_CTRL, FIFO_CTRL_CONFIG) && write_reg(REG_CTRL7, CTRL7_ENABLE_ACCEL_GYRO);
}

#if CONFIG_LAUNCHER_DEVELOPMENT
static bool injected;
static int64_t injected_drain_us;
static imu_sample_t injected_sample = {.ax = IMU_COUNTS_PER_G};

void
imu_inject(const imu_sample_t* sample) {
    portENTER_CRITICAL(&lock);
    if (!injected) {
        injected_drain_us = timing_now_us();
    }
    injected_sample = *sample;
    newest_sequence++;
    injected = true;
    portEXIT_CRITICAL(&lock);
}

void
imu_inject_release(void) {
    portENTER_CRITICAL(&lock);
    injected = false;
    have_newest = false;
    portEXIT_CRITICAL(&lock);
}

static int
read_injected_fifo(imu_sample_t out[IMU_FIFO_SAMPLES], bool* overflowed) {
    portENTER_CRITICAL(&lock);
    if (!injected) {
        portEXIT_CRITICAL(&lock);
        return -1;
    }
    const imu_sample_t sample = injected_sample;
    const int64_t elapsed = (timing_now_us() - injected_drain_us) / IMU_SAMPLE_US;
    injected_drain_us += elapsed * IMU_SAMPLE_US;
    portEXIT_CRITICAL(&lock);
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
        ESP_LOGE(TAG, "No response from 0x%02x", BOARD_IMU_I2C_ADDR);
        return false;
    }
    if (who != WHO_AM_I_VALUE) {
        ESP_LOGE(TAG, "0x%02x answered WHO_AM_I 0x%02x, expected 0x%02x", BOARD_IMU_I2C_ADDR, who, WHO_AM_I_VALUE);
        return false;
    }

    if (!configure(IMU_ODR_CODE)) {
        ESP_LOGE(TAG, "Configuration write failed");
        return false;
    }

    uint8_t revision = 0;
    read_regs(REG_REVISION, &revision, 1);
    const char* model = revision == REVISION_QMI8658C   ? "QMI8658C"
                        : revision == REVISION_QMI8658A ? "QMI8658A"
                                                        : "unknown";
    ESP_LOGI(TAG, "QMI8658 up, revision 0x%02x (%s), +/-8g and +/-512dps", revision, model);

    sensor_present = true;
    ready = true;
    return true;
}

bool
imu_ready(void) {
    return ready;
}

/* Under `lock`: stamps a read of one kind. True when the caller must wake the task after unlocking. */
static bool
stamp_demand(int64_t* read_us) {
    *read_us = timing_now_us();
    const bool wake = parked;
    parked = false;
    return wake;
}

static void
wake_task(bool wake) {
    if (wake) {
        xTaskNotifyGive(task_handle);
    }
}

#if CONFIG_LAUNCHER_DEVELOPMENT
static void
record_reader_age(void) {
    if (have_newest) {
        const int64_t age_us = timing_now_us() - newest_us;
        reader_age_total_us += (uint64_t)age_us;
        reader_age_count++;
        if (age_us > reader_age_max_us) {
            reader_age_max_us = age_us;
        }
    }
}
#endif

bool
imu_read(imu_sample_t* out, uint32_t* sequence) {
    portENTER_CRITICAL(&lock);
    const bool wake = ready && stamp_demand(&demand.newest_us);
    bool available = ready && have_newest;
    if (available) {
        *out = newest;
    }
#if CONFIG_LAUNCHER_DEVELOPMENT
    if (ready && injected) {
        *out = injected_sample;
        available = true;
    }
#endif
    if (available && sequence != NULL) {
        *sequence = newest_sequence;
    }
    portEXIT_CRITICAL(&lock);
    wake_task(wake);
    return available;
}

int
imu_take_fifo(imu_sample_t out[IMU_FIFO_SAMPLES], uint8_t lost_before[IMU_FIFO_SAMPLES], bool* gap) {
    *gap = false;
    if (!ready) {
        return -1;
    }
    portENTER_CRITICAL(&lock);
    const bool wake = stamp_demand(&demand.fifo_us);
    const int count = imu_queue_take(&queue, out, lost_before, gap);
#if CONFIG_LAUNCHER_DEVELOPMENT
    record_reader_age();
#endif
    portEXIT_CRITICAL(&lock);
    wake_task(wake);
    return count;
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

static int
drain_fifo(imu_sample_t out[IMU_FIFO_SAMPLES], int* lost, bool* overflowed) {
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
    if (!sensor_present) {
        return 0;
    }
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

#if CONFIG_LAUNCHER_DEVELOPMENT
/* Closes the window when log_ms has passed. Runs at IMU_TASK_BUS_PRIORITY: logged below the job
 * worker, the line landed inside console_emit_line()'s chunked screenshot rows (A308). */
static void
log_stats(int64_t now_us) {
    const bool window_done = log_ms == 0 || now_us - stats.last_log_us >= (int64_t)log_ms * MATH_US_PER_MS;
    if (!window_done) {
        return;
    }
    portENTER_CRITICAL(&lock);
    const uint64_t age_mean_us = reader_age_count > 0 ? reader_age_total_us / reader_age_count : 0;
    const int64_t age_max_us = reader_age_max_us;
    reader_age_total_us = 0;
    reader_age_max_us = 0;
    reader_age_count = 0;
    portEXIT_CRITICAL(&lock);
    if (log_ms > 0) {
        ESP_LOGI(TAG,
                 "register_reads %u drains %u drain_mean_us %llu drain_max_us %lld delivered %u lost %u "
                 "reader_age_mean_us %llu reader_age_max_us %lld wake_gap_max_us %lld bus_ms %llu parks %u "
                 "parked_ms %llu stack_free_bytes %u",
                 stats.register_reads, stats.drains,
                 (unsigned long long)(stats.drains > 0 ? stats.drain_total_us / stats.drains : 0),
                 (long long)stats.drain_max_us, stats.delivered, stats.lost, (unsigned long long)age_mean_us,
                 (long long)age_max_us, (long long)stats.wake_gap_max_us,
                 (unsigned long long)(stats.bus_us / MATH_US_PER_MS), stats.parks,
                 (unsigned long long)(stats.parked_us / MATH_US_PER_MS), (unsigned)uxTaskGetStackHighWaterMark(NULL));
    }
    stats = (imu_stats_t){.last_log_us = now_us};
}

static void
count_drain(int count, int lost, int64_t elapsed_us) {
    stats.drains++;
    stats.drain_total_us += (uint64_t)elapsed_us;
    if (elapsed_us > stats.drain_max_us) {
        stats.drain_max_us = elapsed_us;
    }
    stats.delivered += count > 0 ? (unsigned)count : 0;
    stats.lost += (unsigned)lost;
}
#endif

static bool
imu_read_or_drain(bool drain_due) {
    static imu_sample_t samples[IMU_FIFO_SAMPLES];
#if CONFIG_LAUNCHER_DEVELOPMENT
    portENTER_CRITICAL(&lock);
    drain_due = drain_due || injected;
    portEXIT_CRITICAL(&lock);
    const int64_t start_us = timing_now_us();
#endif
    if (drain_due) {
        int lost = 0;
        bool gap = false;
        const int count = drain_fifo(samples, &lost, &gap);
        const int64_t now_us = timing_now_us();
        portENTER_CRITICAL(&lock);
#if CONFIG_LAUNCHER_DEVELOPMENT
        const uint32_t sample_us = odr.applied == IMU_BUZZ_ODR_CODE ? IMU_BUZZ_SAMPLE_US : IMU_SAMPLE_US;
        imu_queue_push_drain_timed(&queue, samples, count, lost, gap, now_us, sample_us);
#else
        imu_queue_push_drain(&queue, samples, count, lost, gap);
#endif
        if (count > 0) {
            newest = samples[count - 1];
            have_newest = true;
            newest_sequence++;
#if CONFIG_LAUNCHER_DEVELOPMENT
            newest_us = now_us;
#endif
        }
        portEXIT_CRITICAL(&lock);
#if CONFIG_LAUNCHER_DEVELOPMENT
        count_drain(count, lost, now_us - start_us);
#else
        (void)now_us;
#endif
    } else if (sensor_present) {
        imu_sample_t sample;
#if CONFIG_LAUNCHER_DEVELOPMENT
        stats.register_reads++;
#endif
        if (read_regs(REG_AX_L, (uint8_t*)&sample, sizeof sample)) {
            portENTER_CRITICAL(&lock);
            newest = sample;
            have_newest = true;
            newest_sequence++;
#if CONFIG_LAUNCHER_DEVELOPMENT
            newest_us = timing_now_us();
#endif
            portEXIT_CRITICAL(&lock);
        }
    }
#if CONFIG_LAUNCHER_DEVELOPMENT
    stats.bus_us += (uint64_t)(timing_now_us() - start_us);
#endif
    return drain_due;
}

/* Drains resume on a fresh FIFO: what it held is stale, and the read-mode loss
 * since the last drain was never counted. Queued samples drop behind a gap. */
static void
restart_fifo(void) {
    if (sensor_present) {
        (void)(write_command(CTRL9_RST_FIFO) && write_reg(REG_FIFO_CTRL, FIFO_CTRL_CONFIG));
    }
    fifo_gap = true;
    pending_lost = 0;
    loss_remainder_us = 0;
    portENTER_CRITICAL(&lock);
    imu_queue_push(&queue, NULL, 0, 0, true);
    portEXIT_CRITICAL(&lock);
}

/* The level readers ask for; with none, marks the task parked under the same lock a reader stamps under. */
static imu_demand_level_t
take_demand(void) {
    portENTER_CRITICAL(&lock);
    const imu_demand_level_t level = imu_demand_level(&demand, timing_now_us());
    parked = level == IMU_DEMAND_NONE;
    portEXIT_CRITICAL(&lock);
    return level;
}

#if CONFIG_LAUNCHER_DEVELOPMENT
/* Longest a request waits: two of the slowest task periods the tunables allow. */
#define IMU_ODR_WAIT_MS (2 * IMU_PERIOD_MAX_MS)

/* The bus work runs outside `lock` on a copy; imu_set_odr() only waits on it meanwhile.
 * Any reconfigure resets the FIFO, so a gap drops queued samples spaced at the old rate. */
static void
apply_odr_request(void) {
    portENTER_CRITICAL(&lock);
    imu_odr_t next = odr;
    portEXIT_CRITICAL(&lock);
    const imu_odr_outcome_t outcome = imu_odr_apply(&next, configure, &fifo_gap);
    if (outcome == IMU_ODR_UNCHANGED) {
        return;
    }
    if (outcome == IMU_ODR_LOST) {
        ESP_LOGE(TAG, "ODR switch to %u and restore of %u both failed; the sensor may be off", odr.requested,
                 next.applied);
    }
    pending_lost = 0;
    loss_remainder_us = 0;
    portENTER_CRITICAL(&lock);
    imu_queue_set_rate(&queue, next.applied);
    odr = next;
    portEXIT_CRITICAL(&lock);
}

int
imu_take_fifo_at(unsigned odr_code, imu_sample_t out[IMU_FIFO_SAMPLES], uint8_t lost_before[IMU_FIFO_SAMPLES],
                 bool* gap) {
    *gap = false;
    if (!ready) {
        return -1;
    }
    portENTER_CRITICAL(&lock);
    const bool wake = stamp_demand(&demand.fifo_us);
    const int count = imu_queue_take_at(&queue, odr_code, out, lost_before, gap);
    portEXIT_CRITICAL(&lock);
    wake_task(wake);
    return count;
}

int
imu_take_fifo_timed_at(unsigned odr_code, imu_sample_t out[IMU_FIFO_SAMPLES], uint8_t lost_before[IMU_FIFO_SAMPLES],
                       bool* gap, int64_t sample_us[IMU_FIFO_SAMPLES]) {
    *gap = false;
    if (!ready) {
        return -1;
    }
    portENTER_CRITICAL(&lock);
    const bool wake = stamp_demand(&demand.fifo_us);
    const int count = imu_queue_take_timed_at(&queue, odr_code, out, lost_before, gap, sample_us);
    portEXIT_CRITICAL(&lock);
    wake_task(wake);
    return count;
}

bool
imu_set_odr(unsigned odr_code) {
    if (!sensor_present || task_handle == NULL) {
        return false;
    }
    portENTER_CRITICAL(&lock);
    odr.requested = odr_code;
    const bool wake = stamp_demand(&demand.fifo_us);
    portEXIT_CRITICAL(&lock);
    wake_task(wake);
    const int64_t deadline = timing_now_us() + ((int64_t)IMU_ODR_WAIT_MS * MATH_US_PER_MS);
    for (;;) {
        portENTER_CRITICAL(&lock);
        const unsigned applied = odr.applied;
        const unsigned requested = odr.requested;
        portEXIT_CRITICAL(&lock);
        if (applied == odr_code || requested != odr_code || timing_now_us() >= deadline) {
            return applied == odr_code;
        }
        timing_sleep_ms(IMU_PERIOD_MIN_MS);
    }
}
#endif

/* Sleeps until a reader stamps demand; a read since take_demand() leaves the notification pending. */
static void
park_until_read(void) {
#if CONFIG_LAUNCHER_DEVELOPMENT
    stats.parks++;
    const int64_t parked_us = timing_now_us();
#endif
    (void)ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
#if CONFIG_LAUNCHER_DEVELOPMENT
    stats.last_wake_us = timing_now_us();
    stats.parked_us += (uint64_t)(stats.last_wake_us - parked_us);
#endif
}

#if CONFIG_LAUNCHER_DEVELOPMENT
static void
count_wake_gap(void) {
    const int64_t now_us = timing_now_us();
    const int64_t wake_gap_us = now_us - stats.last_wake_us;
    if (wake_gap_us > stats.wake_gap_max_us) {
        stats.wake_gap_max_us = wake_gap_us;
    }
    stats.last_wake_us = now_us;
}
#endif

/* One period's bus work at `asked`: entering FIFO restarts it, then a register read or a due drain.
 * The bus lock has no priority inheritance: a preempted drain holds the bus
 * and keeps the FIFO in read mode, discarding arrivals. */
static void
serve_demand(imu_demand_level_t asked, imu_demand_level_t* level, unsigned* read_ticks, TickType_t drain_ticks) {
    vTaskPrioritySet(NULL, IMU_TASK_BUS_PRIORITY);
#if CONFIG_LAUNCHER_DEVELOPMENT
    if (sensor_present) {
        apply_odr_request();
    }
#endif
    if (asked == IMU_DEMAND_FIFO && *level != IMU_DEMAND_FIFO) {
        restart_fifo();
        *read_ticks = 0;
    }
    *level = asked;
    if (imu_read_or_drain(asked == IMU_DEMAND_FIFO && *read_ticks >= drain_ticks)) {
        *read_ticks = 0;
    }
    (*read_ticks)++;
#if CONFIG_LAUNCHER_DEVELOPMENT
    log_stats(timing_now_us());
#endif
    vTaskPrioritySet(NULL, IMU_TASK_PRIORITY);
}

static void
imu_task(void* arg) {
    (void)arg;
    TickType_t last_wake = xTaskGetTickCount();
    unsigned read_ticks = 0;
    imu_demand_level_t level = IMU_DEMAND_NONE;
#if CONFIG_LAUNCHER_DEVELOPMENT
    stats.last_wake_us = timing_now_us();
#endif
    while (1) {
        const imu_demand_level_t asked = take_demand();
        if (asked == IMU_DEMAND_NONE) {
            park_until_read();
            level = IMU_DEMAND_NONE;
            last_wake = xTaskGetTickCount();
            continue;
        }
#if CONFIG_LAUNCHER_DEVELOPMENT
        count_wake_gap();
#endif
        unsigned read_period_ms = read_ms;
        unsigned drain_period_ms = drain_ms;
#if CONFIG_LAUNCHER_DEVELOPMENT
        read_period_ms = imu_odr_period_ms(odr.applied, read_period_ms);
        drain_period_ms = imu_odr_period_ms(odr.applied, drain_period_ms);
#endif
        const TickType_t requested_period = pdMS_TO_TICKS(read_period_ms);
        const TickType_t period = requested_period > 0 ? requested_period : 1;
        serve_demand(asked, &level, &read_ticks, pdMS_TO_TICKS(drain_period_ms) / period);
        if (xTaskDelayUntil(&last_wake, period) == pdFALSE) {
            last_wake = xTaskGetTickCount();
        }
    }
}

void
imu_start(void) {
#if CONFIG_LAUNCHER_QEMU
    if (!ready) {
        ESP_LOGW(TAG, "No sensor at 0x%02x; samples come from imu_inject()", BOARD_IMU_I2C_ADDR);
        ready = true;
        injected = true;
        injected_drain_us = timing_now_us();
    }
#endif
    if (!ready || task_handle != NULL) {
        return;
    }
    if (xTaskCreatePinnedToCore(imu_task, "imu", IMU_TASK_STACK_BYTES, NULL, IMU_TASK_PRIORITY, &task_handle,
                                IMU_TASK_CORE)
        != pdPASS) {
        ESP_LOGE(TAG, "Could not start the IMU task (largest free block is %u bytes); motion will not work",
                 (unsigned)memory_largest_block(MEMORY_8BIT));
    }
}
