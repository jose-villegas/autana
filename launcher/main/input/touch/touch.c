#include "input/touch/touch.h"
#include "input/touch/touch_calib.h"
#include "input/touch/touch_fsm.h"
#include "input/touch/touch_inject_fsm.h"

#include "build/build_variant.h"
#include "core/memory.h"
#include "core/timing.h"
#include "core/tune.h"

#include "bsp/esp-bsp.h"
#include "bsp/touch.h"
#include "driver/gpio.h"
#include "esp_attr.h"
#include "esp_lcd_touch.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char* TAG = "touch";

static esp_lcd_touch_handle_t panel;

/* How this panel reports a tap aimed at a known point, fitted from 217 taps
 * across both orientations: the long axis alone reads 18% stretched about a
 * point 165 px down it, the same whichever way the board is held. */
static const touch_calib_fit_t PANEL_FIT = {
    .xx = 1.119F,
    .xy = 0.019F,
    .x0 = -23.1F,
    .yx = 0.015F,
    .yy = 1.183F,
    .y0 = -29.2F,
};
static touch_calib_t calib;

TUNE_OWNER(touch);
TUNE(touch, calibrate, 1, 0, 1);

/* All the interpretation lives in touch_fsm, which is hardware-free and
 * covered by host tests. This file is only responsible for getting samples out
 * of the controller and handing them over. */
static touch_fsm_t fsm;

/* Shared between the polling task and the render loop. A spinlock-guarded
 * critical section is correct on either a one-core or two-core target, and
 * cheap here since it only ever spans a few field updates. */
static portMUX_TYPE lock = portMUX_INITIALIZER_UNLOCKED;

/* Set from the INT falling edge. The CST820 signals each report with a ~1 ms
 * pulse rather than holding INT low while touched, so sampling the level at
 * TOUCH_POLL_HZ alone sees about one report in ten. */
static volatile bool report_pending;

/* Whether the last read found a finger: a controller that pulses only on
 * change must still be read while a finger rests, or the FSM sees a lift. */
static bool was_touching;

/* Woken by the INT edge so a new contact is read at once instead of at the
 * next poll tick. NULL until the task exists, and when it could not start. */
static TaskHandle_t touch_task_handle;

#if CONFIG_LAUNCHER_DEVELOPMENT
static uint32_t point_samples, moved_samples;
static vec2i_t last_point = {-1, -1};

/* Reads the controller every poll even with no contact, so a board nobody
 * touches still measures how long a touch read waits for the shared bus. */
TUNE(touch, probe, 0, 0, 1);

/* Controller read times since the last touch_take_read_times(): a histogram for
 * the p95, the exact worst. The read includes its wait for the shared I2C bus. */
#define TOUCH_READ_BUCKET_US 250
#define TOUCH_READ_BUCKETS   128

typedef struct {
    uint32_t buckets[TOUCH_READ_BUCKETS];
    int64_t max_us;
    unsigned count;
} touch_read_times_t;

static touch_read_times_t read_times;

static void
count_read_time(int64_t read_us) {
    const int64_t bucket = read_us / TOUCH_READ_BUCKET_US;
    portENTER_CRITICAL(&lock);
    read_times.buckets[bucket < TOUCH_READ_BUCKETS ? bucket : TOUCH_READ_BUCKETS - 1]++;
    read_times.max_us = read_us > read_times.max_us ? read_us : read_times.max_us;
    read_times.count++;
    portEXIT_CRITICAL(&lock);
}
#endif

/* Need not be cache-off safe: esp_lcd_touch installs the GPIO ISR service with flags 0.
 * Kept in IRAM until a touch-latency measurement says flash is fine. IRAM budget: 50 B */
static void IRAM_ATTR
on_touch_int(esp_lcd_touch_handle_t tp) {
    (void)tp;
    report_pending = true;
    if (touch_task_handle != NULL) {
        BaseType_t woken = pdFALSE;
        vTaskNotifyGiveFromISR(touch_task_handle, &woken);
        portYIELD_FROM_ISR(woken);
    }
}

#if CONFIG_LAUNCHER_DEVELOPMENT
static bool injected_down;
static bool injected_release;
static vec2i_t injected_point;
static touch_inject_t injected_contact;
static touch_inject_completion_t inject_completion;
static touch_inject_completion_t completed_inject;

/* Returns true when injection supplied this poll's result, including its
 * final no-contact sample. That lets touch_fsm see the lift before the panel
 * before the controller becomes the source again. */
static bool
poll_injected(int64_t now_us, bool* have_point, vec2i_t* point) {
    portENTER_CRITICAL(&lock);
    if (injected_release) {
        injected_release = false;
        *have_point = false;
        portEXIT_CRITICAL(&lock);
        return true;
    }
    if (injected_down) {
        *have_point = true;
        *point = injected_point;
        portEXIT_CRITICAL(&lock);
        return true;
    }
    if (injected_contact.active) {
        *have_point = touch_inject_step(&injected_contact, now_us, point);
        if (!*have_point) {
            completed_inject = inject_completion;
        }
        portEXIT_CRITICAL(&lock);
        return true;
    }
    portEXIT_CRITICAL(&lock);
#if CONFIG_LAUNCHER_QEMU
    *have_point = false;
    return true;
#else
    return false;
#endif
}

void
touch_inject(bool down, vec2i_t point) {
    portENTER_CRITICAL(&lock);
    injected_contact.active = false;
    injected_down = down;
    injected_point = point;
    injected_release = !down;
    portEXIT_CRITICAL(&lock);
}

void
touch_inject_start(vec2i_t from, vec2i_t to, uint32_t ms, touch_inject_completion_t completion) {
    portENTER_CRITICAL(&lock);
    injected_down = false;
    injected_release = false;
    touch_inject_init(&injected_contact, from, to, ms);
    inject_completion = completion;
    portEXIT_CRITICAL(&lock);
}

bool
touch_inject_take_completion(touch_inject_completion_t* completion) {
    portENTER_CRITICAL(&lock);
    if (completed_inject == TOUCH_INJECT_NONE) {
        portEXIT_CRITICAL(&lock);
        return false;
    }
    *completion = completed_inject;
    completed_inject = TOUCH_INJECT_NONE;
    portEXIT_CRITICAL(&lock);
    return true;
}
#endif

static void
poll_controller(bool* have_point, vec2i_t* point) {
    /* Only talk to the controller when it has something: a controller NACKs
     * register reads while idle, and each failed transaction costs a bus
     * timeout; polling blindly at this rate would swamp the system. */
    const bool pending = report_pending;
    report_pending = false;
#if CONFIG_LAUNCHER_DEVELOPMENT
    const bool probing = probe != 0;
#else
    const bool probing = false;
#endif
    if (panel != NULL && (pending || was_touching || probing || gpio_get_level(BSP_LCD_TOUCH_INT) == 0)) {
#if CONFIG_LAUNCHER_DEVELOPMENT
        const int64_t start_us = timing_now_us();
        const esp_err_t read = esp_lcd_touch_read_data(panel);
        count_read_time(timing_now_us() - start_us);
#else
        const esp_err_t read = esp_lcd_touch_read_data(panel);
#endif
        if (read == ESP_OK) {
            esp_lcd_touch_point_data_t raw = {0};
            uint8_t count = 0;
            if (esp_lcd_touch_get_data(panel, &raw, &count, 1) == ESP_OK && count > 0) {
                *have_point = true;
                *point = (vec2i_t){raw.x, raw.y};
            }
        }
    }
}

static void
poll_once(void) {
    bool have_point = false;
    vec2i_t point = {0};
    const int64_t now_us = timing_now_us();

#if CONFIG_LAUNCHER_DEVELOPMENT
    const bool injected = poll_injected(now_us, &have_point, &point);
    if (!injected) {
        poll_controller(&have_point, &point);
    }
#else
    poll_controller(&have_point, &point);
#endif
#if CONFIG_LAUNCHER_DEVELOPMENT
    touch_calib_prepare_point(injected, have_point, &point, calibrate, &calib, BSP_LCD_H_RES, BSP_LCD_V_RES);
#else
    touch_calib_prepare_point(false, have_point, &point, calibrate, &calib, BSP_LCD_H_RES, BSP_LCD_V_RES);
#endif

    portENTER_CRITICAL(&lock);
    touch_fsm_update(&fsm, have_point, point, now_us);
    was_touching = have_point;

#if CONFIG_LAUNCHER_DEVELOPMENT
    if (have_point) {
        point_samples++;
        moved_samples += !vec2i_equal(point, last_point) ? 1U : 0U;
        last_point = point;
    }
#endif
    portEXIT_CRITICAL(&lock);
}

static void
touch_task(void* arg) {
    const TickType_t period = pdMS_TO_TICKS(1000 / TOUCH_POLL_HZ);
    TickType_t last_wake = xTaskGetTickCount();

    while (1) {
        poll_once();
        const TickType_t ticks = period > 0 ? period : 1;
        if (was_touching) {
            /* A resting finger is sampled at the poll rate, however often
             * the controller pulses. */
            xTaskDelayUntil(&last_wake, ticks);
        } else {
            /* Idle: a contact wakes the task, the timeout still polls the
             * level when no interrupt is registered. */
            ulTaskNotifyTake(pdTRUE, ticks);
            last_wake = xTaskGetTickCount();
        }
    }
}

void
touch_start(void) {
    static bool started;
    if (started) {
        return;
    }
    started = true;

    touch_fsm_init(&fsm);
    calib = touch_calib_from_fit(PANEL_FIT);

    if (bsp_touch_new(NULL, &panel) != ESP_OK) {
#if CONFIG_LAUNCHER_QEMU
        ESP_LOGW(TAG, "No touch controller; input comes from touch_inject()");
#else
        ESP_LOGW(TAG, "Touch controller unavailable; input will not work");
#endif
        panel = NULL;
    } else if (esp_lcd_touch_register_interrupt_callback(panel, on_touch_int) != ESP_OK) {
        ESP_LOGW(TAG, "No touch interrupt; falling back to sampling INT's level");
    }

    /* Above the render loop's priority so a long blit cannot delay sampling,
     * the entire point of running it separately. The result is CHECKED: on
     * an autorun self-test image, touch_start() runs after the test
     * suite has allocated and freed the heap into a state with no 3 KB run
     * left, so this call can fail and must not fail silently. */
    if (xTaskCreate(touch_task, "touch", 3072, NULL, 6, &touch_task_handle) != pdPASS) {
        ESP_LOGE(TAG,
                 "Could not start the touch task (largest free block "
                 "is %u bytes); input will not work",
                 (unsigned)memory_largest_block(MEMORY_8BIT));
    }
}

void
touch_read(input_t* out) {
    portENTER_CRITICAL(&lock);
    touch_fsm_take(&fsm, out);
    portEXIT_CRITICAL(&lock);
}

#if CONFIG_LAUNCHER_DEVELOPMENT
void
touch_take_sample_counts(uint32_t* points, uint32_t* moved) {
    portENTER_CRITICAL(&lock);
    *points = point_samples;
    *moved = moved_samples;
    point_samples = 0;
    moved_samples = 0;
    portEXIT_CRITICAL(&lock);
}

void
touch_take_read_times(unsigned* reads, int64_t* p95_us, int64_t* max_us) {
    portENTER_CRITICAL(&lock);
    const touch_read_times_t taken = read_times;
    read_times = (touch_read_times_t){0};
    portEXIT_CRITICAL(&lock);
    /* The p95 is the upper edge of the bucket holding the 95th-percentile read. */
    const unsigned rank = (taken.count * 95U + 99U) / 100U;
    unsigned seen = 0;
    int bucket = 0;
    while (bucket < TOUCH_READ_BUCKETS - 1 && seen + taken.buckets[bucket] < rank) {
        seen += taken.buckets[bucket];
        bucket++;
    }
    *reads = taken.count;
    *p95_us = taken.count > 0 ? (int64_t)(bucket + 1) * TOUCH_READ_BUCKET_US : 0;
    *max_us = taken.max_us;
}
#endif
