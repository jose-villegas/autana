/*
 * imu: the polling task over the chip the board carries (imu_chip.h).
 */

#include "input/motion/imu.h"

#include <string.h>

#include "core/job.h"
#include "core/memory.h"
#include "core/timing.h"
#include "core/tune.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "input/motion/imu_chip.h"
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

static bool ready;
static bool sensor_present;
/* An injection fed the drains: what the chip's FIFO holds is stale once it hands back. */
static bool restart_due;

#if CONFIG_LAUNCHER_DEVELOPMENT
static bool injected;
static int64_t injected_drain_us;
static imu_sample_t injected_sample = IMU_SAMPLE_UPRIGHT;

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

    i2c_master_bus_handle_t bus = board_i2c_bus();
    if (bus == NULL) {
        ESP_LOGE(TAG, "I2C bus is not initialised");
        return false;
    }

    if (!BOARD_IMU_CHIP.open(bus, BOARD_IMU_I2C_ADDR, IMU_ODR_CODE)) {
        return false;
    }

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

#if CONFIG_LAUNCHER_DEVELOPMENT
/* A FIFO burst holding a gyro rate within IMU_SUSPECT_MARGIN_DPS of full scale is logged raw,
 * with a register sample beside it, IMU_SUSPECT_LOGS times a boot at most: at rest, such a rate
 * is a bad frame, and the bytes tell a shifted frame from a bad axis. */
#define IMU_SUSPECT_MARGIN_DPS 8
#define IMU_SUSPECT_COUNTS     ((int)((IMU_GYRO_RANGE_DPS - IMU_SUSPECT_MARGIN_DPS) * IMU_COUNTS_PER_DPS))
#define IMU_SUSPECT_LOGS       8

static bool
suspect_rate(int16_t rate) {
    return rate > IMU_SUSPECT_COUNTS || rate < -IMU_SUSPECT_COUNTS;
}

static void
log_suspect_burst(const imu_sample_t* samples, int count) {
    static unsigned logged;
    int first = -1;
    for (int i = 0; i < count && first < 0; i++) {
        if (suspect_rate(samples[i].gx) || suspect_rate(samples[i].gy) || suspect_rate(samples[i].gz)) {
            first = i;
        }
    }
    if (first < 0 || logged >= IMU_SUSPECT_LOGS) {
        return;
    }
    logged++;
    imu_sample_t reg = {0};
    const bool reg_ok = BOARD_IMU_CHIP.read(&reg);
    ESP_LOGW(TAG, "gyro near full scale in a FIFO burst: count %d first %d, register%s %d %d %d %d %d %d", count, first,
             reg_ok ? "" : " (failed)", reg.ax, reg.ay, reg.az, reg.gx, reg.gy, reg.gz);
    ESP_LOG_BUFFER_HEX_LEVEL(TAG, samples, (size_t)count * sizeof *samples, ESP_LOG_WARN);
}
#endif

/* Drains resume on a fresh FIFO: what it held is stale, and the read-mode loss
 * since the last drain was never counted. Queued samples drop behind a gap. */
static void
restart_fifo(void) {
    if (sensor_present) {
        BOARD_IMU_CHIP.restart();
    }
    portENTER_CRITICAL(&lock);
    imu_queue_push(&queue, NULL, 0, 0, true);
#if CONFIG_LAUNCHER_DEVELOPMENT
    odr.flowing = false;
#endif
#if CONFIG_LAUNCHER_DEVELOPMENT
    odr.flowing = false;
#endif
    portEXIT_CRITICAL(&lock);
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
        restart_due = true;
        return injected_count;
    }
#endif
    if (!sensor_present) {
        return 0;
    }
    if (restart_due) {
        restart_due = false;
        restart_fifo();
    }
    const int count = BOARD_IMU_CHIP.drain(out, lost, overflowed);
#if CONFIG_LAUNCHER_DEVELOPMENT
    if (count > 0) {
        log_suspect_burst(out, count);
    }
#endif
    return count;
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
    stats = (imu_stats_t){.last_log_us = now_us, .last_wake_us = stats.last_wake_us};
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
            odr.flowing = true;
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
        if (BOARD_IMU_CHIP.read(&sample)) {
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
static uint32_t
apply_odr_request(imu_demand_level_t* level, unsigned* read_ticks) {
    portENTER_CRITICAL(&lock);
    imu_odr_t next = odr;
    portEXIT_CRITICAL(&lock);
    const imu_odr_outcome_t outcome = imu_odr_apply(&next, BOARD_IMU_CHIP.configure, level);
    if (outcome == IMU_ODR_UNCHANGED) {
        return next.sequence;
    }
    if (outcome == IMU_ODR_LOST) {
        ESP_LOGE(TAG, "ODR switch to %u and restore of %u both failed; the sensor may be off", odr.requested,
                 next.applied);
    }
    *read_ticks = 0;
    portENTER_CRITICAL(&lock);
    imu_queue_set_rate(&queue, next.applied);
    odr.applied = next.applied;
    odr.flowing = false;
    if (odr.sequence == next.sequence) {
        odr.requested = next.requested;
    }
    portEXIT_CRITICAL(&lock);
    return next.sequence;
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
    const uint32_t sequence = ++odr.sequence;
    const bool wake = stamp_demand(&demand.fifo_us);
    portEXIT_CRITICAL(&lock);
    wake_task(wake);
    const int64_t deadline = timing_now_us() + ((int64_t)IMU_ODR_WAIT_MS * MATH_US_PER_MS);
    for (;;) {
        portENTER_CRITICAL(&lock);
        const unsigned applied = odr.applied;
        const bool served = imu_odr_ready(&odr, sequence);
        const bool superseded = odr.sequence != sequence;
        portEXIT_CRITICAL(&lock);
        if (served || superseded || timing_now_us() >= deadline) {
            return served && applied == odr_code;
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
 * The bus lock has no priority inheritance: a preempted transaction holds the bus,
 * and a preempted drain keeps the FIFO in read mode, discarding arrivals. */
static void
serve_demand(imu_demand_level_t asked, imu_demand_level_t* level, unsigned* read_ticks, TickType_t drain_ticks) {
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
        vTaskPrioritySet(NULL, IMU_TASK_BUS_PRIORITY);
#if CONFIG_LAUNCHER_DEVELOPMENT
        uint32_t served_sequence = 0;
        if (sensor_present && asked == IMU_DEMAND_FIFO) {
            served_sequence = apply_odr_request(&level, &read_ticks);
        }
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
#if CONFIG_LAUNCHER_DEVELOPMENT
        if (sensor_present && asked == IMU_DEMAND_FIFO) {
            portENTER_CRITICAL(&lock);
            odr.served_sequence = served_sequence;
            portEXIT_CRITICAL(&lock);
        }
#endif
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
