#include "input/touch.h"
#include "input/touch_fsm.h"

#include "build_variant.h"

#include "bsp/esp-bsp.h"
#include "bsp/touch.h"
#include "driver/gpio.h"
#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "esp_lcd_touch.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char* TAG = "touch";

static esp_lcd_touch_handle_t panel;

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

#if CONFIG_LAUNCHER_DEVELOPMENT
static uint32_t point_samples, moved_samples;
static int last_x = -1, last_y = -1;
#endif

static void IRAM_ATTR
on_touch_int(esp_lcd_touch_handle_t tp) {
    (void)tp;
    report_pending = true;
}

#if CONFIG_LAUNCHER_QEMU
/* A controller with no chip behind it: it reports whatever touch_inject()
 * last set, through the same driver interface a real one answers. */
static volatile bool injected_down;
static volatile uint16_t injected_x, injected_y;

static esp_err_t
injected_read_data(esp_lcd_touch_handle_t tp) {
    (void)tp;
    return ESP_OK;
}

static bool
injected_get_xy(esp_lcd_touch_handle_t tp, uint16_t* x, uint16_t* y, uint16_t* strength, uint8_t* point_num,
                uint8_t max_point_num) {
    (void)tp;
    (void)strength;
    (void)max_point_num;
    *point_num = injected_down ? 1 : 0;
    x[0] = injected_x;
    y[0] = injected_y;
    return injected_down;
}

static esp_lcd_touch_t injected_panel = {
    .read_data = injected_read_data,
    .get_xy = injected_get_xy,
};

void
touch_inject(bool down, int x, int y) {
    injected_x = (uint16_t)x;
    injected_y = (uint16_t)y;
    injected_down = down;
    report_pending = true;
}
#endif

static void
poll_once(void) {
    bool have_point = false;
    int x = 0, y = 0;

    /* Only talk to the controller when it has something: a controller NACKs
     * register reads while idle, and each failed transaction costs a bus
     * timeout - polling blindly at this rate would swamp the system. */
    const bool pending = report_pending;
    report_pending = false;
    if (panel != NULL && (pending || was_touching || gpio_get_level(BSP_LCD_TOUCH_INT) == 0)) {
        if (esp_lcd_touch_read_data(panel) == ESP_OK) {
            esp_lcd_touch_point_data_t point = {0};
            uint8_t count = 0;
            if (esp_lcd_touch_get_data(panel, &point, &count, 1) == ESP_OK && count > 0) {
                have_point = true;
                x = point.x;
                y = point.y;
            }
        }
    }
    was_touching = have_point;

    const int64_t now_us = esp_timer_get_time();

    portENTER_CRITICAL(&lock);
    touch_fsm_update(&fsm, have_point, x, y, now_us);
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
        vTaskDelayUntil(&last_wake, period > 0 ? period : 1);
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

    if (bsp_touch_new(NULL, &panel) != ESP_OK) {
#if CONFIG_LAUNCHER_QEMU
        ESP_LOGW(TAG, "No touch controller; input comes from touch_inject()");
        panel = &injected_panel;
#else
        ESP_LOGW(TAG, "Touch controller unavailable; input will not work");
        panel = NULL;
#endif
    } else if (esp_lcd_touch_register_interrupt_callback(panel, on_touch_int) != ESP_OK) {
        ESP_LOGW(TAG, "No touch interrupt; falling back to sampling INT's level");
    }

    /* Above the render loop's priority so a long blit cannot delay sampling -
     * the entire point of running it separately. The result is CHECKED: on
     * an autorun self-test image, touch_start() runs after the test
     * suite has allocated and freed the heap into a state with no 3 KB run
     * left, so this call can fail and must not fail silently. */
    if (xTaskCreate(touch_task, "touch", 3072, NULL, 6, NULL) != pdPASS) {
        ESP_LOGE(TAG,
                 "Could not start the touch task (largest free block "
                 "is %u bytes); input will not work",
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
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
