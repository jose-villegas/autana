#include "input/touch.h"
#include "input/touch_calib.h"
#include "input/touch_fsm.h"
#include "input/touch_inject_fsm.h"
#include "input/touch_point.h"

#include "build_variant.h"
#include "util/memory.h"
#include "util/timing.h"
#include "util/tune.h"

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
    .xx = 1.119f,
    .xy = 0.019f,
    .x0 = -23.1f,
    .yx = 0.015f,
    .yy = 1.183f,
    .y0 = -29.2f,
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
static int last_x = -1, last_y = -1;
#endif

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
static int injected_x, injected_y;
static touch_inject_t injected_gesture;
static touch_gesture_completion_t gesture_completion;
static touch_gesture_completion_t completed_gesture;

/* Returns true when injection supplied this poll's result, including its
 * final no-contact sample. That lets touch_fsm see the lift before the panel
 * before the controller becomes the source again. */
static bool
poll_injected(int64_t now_us, bool* have_point, int* x, int* y) {
    portENTER_CRITICAL(&lock);
    if (injected_release) {
        injected_release = false;
        *have_point = false;
        portEXIT_CRITICAL(&lock);
        return true;
    }
    if (injected_down) {
        *have_point = true;
        *x = injected_x;
        *y = injected_y;
        portEXIT_CRITICAL(&lock);
        return true;
    }
    if (injected_gesture.active) {
        *have_point = touch_inject_step(&injected_gesture, now_us, x, y);
        if (!*have_point) {
            completed_gesture = gesture_completion;
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
    injected_gesture.active = false;
    injected_down = down;
    injected_x = point.x;
    injected_y = point.y;
    injected_release = !down;
    portEXIT_CRITICAL(&lock);
}

void
touch_gesture_start(int x0, int y0, int x1, int y1, uint32_t ms, touch_gesture_completion_t completion) {
    portENTER_CRITICAL(&lock);
    injected_down = false;
    injected_release = false;
    touch_inject_init(&injected_gesture, x0, y0, x1, y1, ms);
    gesture_completion = completion;
    portEXIT_CRITICAL(&lock);
}

bool
touch_gesture_take_completion(touch_gesture_completion_t* completion) {
    portENTER_CRITICAL(&lock);
    if (completed_gesture == TOUCH_GESTURE_NONE) {
        portEXIT_CRITICAL(&lock);
        return false;
    }
    *completion = completed_gesture;
    completed_gesture = TOUCH_GESTURE_NONE;
    portEXIT_CRITICAL(&lock);
    return true;
}
#endif

static void
poll_controller(bool* have_point, int* x, int* y) {
    /* Only talk to the controller when it has something: a controller NACKs
     * register reads while idle, and each failed transaction costs a bus
     * timeout; polling blindly at this rate would swamp the system. */
    const bool pending = report_pending;
    report_pending = false;
    if (panel != NULL && (pending || was_touching || gpio_get_level(BSP_LCD_TOUCH_INT) == 0)) {
        if (esp_lcd_touch_read_data(panel) == ESP_OK) {
            esp_lcd_touch_point_data_t point = {0};
            uint8_t count = 0;
            if (esp_lcd_touch_get_data(panel, &point, &count, 1) == ESP_OK && count > 0) {
                *have_point = true;
                *x = point.x;
                *y = point.y;
            }
        }
    }
}

static void
poll_once(void) {
    bool have_point = false;
    int x = 0, y = 0;
    const int64_t now_us = timing_now_us();

#if CONFIG_LAUNCHER_DEVELOPMENT
    const bool injected = poll_injected(now_us, &have_point, &x, &y);
    if (!injected) {
        poll_controller(&have_point, &x, &y);
    }
#else
    poll_controller(&have_point, &x, &y);
#endif
#if CONFIG_LAUNCHER_DEVELOPMENT
    touch_point_prepare(injected, have_point, &x, &y, calibrate, &calib, BSP_LCD_H_RES, BSP_LCD_V_RES);
#else
    touch_point_prepare(false, have_point, &x, &y, calibrate, &calib, BSP_LCD_H_RES, BSP_LCD_V_RES);
#endif

    portENTER_CRITICAL(&lock);
    touch_fsm_update(&fsm, have_point, (vec2i_t){x, y}, now_us);
    was_touching = have_point;

#if CONFIG_LAUNCHER_DEVELOPMENT
    if (have_point) {
        point_samples++;
        moved_samples += (x != last_x || y != last_y) ? 1u : 0u;
        last_x = x;
        last_y = y;
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
            vTaskDelayUntil(&last_wake, ticks);
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
#endif
