#include "input/buttons/buttons.h"
#include "input/buttons/button_fsm.h"

#include "board/board.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "core/timing.h"

#include "power/power.h"

/* 50 Hz. Fast enough that a press never feels missed, slow enough that the I2C
 * read is nothing next to the rest of the frame, and deliberately decoupled
 * from the render loop, which now runs at up to 1000 fps and would hammer the
 * shared bus if it polled the PMU itself. */
#define POLL_HZ 50

static button_fsm_t boot_fsm;

/* PWR is an event, nothing to debounce: just flags waiting to be
 * collected, one per interrupt bit below. The PMU itself decides "held",
 * timing the press against its own irqlevel threshold, so there is no local
 * hold-timer state to keep the way BOOT's button_fsm_t needs one. */
static bool power_pressed;
static bool power_held;

/* Shared between the polling task and the render loop. A spinlock-guarded
 * critical section is correct on either a one-core or two-core target, and
 * cheap here since it only ever spans a few field updates. */
static portMUX_TYPE lock = portMUX_INITIALIZER_UNLOCKED;

/* Polling */

static void
poll_once(void) {
    const bool boot_down = gpio_get_level(BOARD_BOOT_GPIO) == 0; /* active low */
    const power_key_events_t events = power_poll();
    const int64_t now_us = timing_now_us();

    portENTER_CRITICAL(&lock);
    button_fsm_update(&boot_fsm, boot_down, now_us);
    if (events.short_press) {
        power_pressed = true;
    }
    if (events.long_press) {
        power_held = true;
    }
    portEXIT_CRITICAL(&lock);
}

static void
buttons_task(void* arg) {
    (void)arg;

    TickType_t last_wake = xTaskGetTickCount();
    const TickType_t period = pdMS_TO_TICKS(1000 / POLL_HZ);

    while (1) {
        poll_once();
        xTaskDelayUntil(&last_wake, period > 0 ? period : 1);
    }
}

void
buttons_start(void) {
    const gpio_config_t boot = {
        .pin_bit_mask = 1ULL << BOARD_BOOT_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&boot);

    button_fsm_reset(&boot_fsm);
    power_start();

    xTaskCreate(buttons_task, "buttons", 3072, NULL, 5, NULL);
}

void
buttons_read(button_t* boot, button_t* power) {
    portENTER_CRITICAL(&lock);

    boot->down = button_fsm_is_down(&boot_fsm);
    boot->pressed = button_fsm_take_pressed(&boot_fsm);
    boot->released = button_fsm_take_released(&boot_fsm);
    boot->held = button_fsm_take_held(&boot_fsm);

    /* Not a level because the PMU's edge interrupts are not enabled, not
     * because it cannot report one; see buttons.h. */
    power->down = false;
    power->pressed = power_pressed;
    power->released = false;
    power->held = power_held;
    power_pressed = false;
    power_held = false;

    portEXIT_CRITICAL(&lock);
}

#if CONFIG_LAUNCHER_DEVELOPMENT
void
buttons_inject(buttons_inject_button_t button, bool held) {
    portENTER_CRITICAL(&lock);
    if (button == BUTTONS_INJECT_BOOT) {
        const int64_t now_us = timing_now_us();
        button_fsm_update(&boot_fsm, true, now_us);
        button_fsm_update(&boot_fsm, true, now_us + BUTTON_DEBOUNCE_US);
        if (held) {
            button_fsm_update(&boot_fsm, true, now_us + BUTTON_DEBOUNCE_US + BUTTON_HOLD_US);
        }
    } else if (held) {
        power_held = true;
    } else {
        power_pressed = true;
    }
    portEXIT_CRITICAL(&lock);
}
#endif
