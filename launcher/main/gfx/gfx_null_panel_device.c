#include "gfx/gfx_null_panel.h"

#include <stdbool.h>
#include <stdint.h>

#include "gfx/gfx.h"
#include "gfx/gfx_dirty.h"

#include "esp_lcd_panel_interface.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"

#define QSPI_DATA_LINES 4

/* As many strips as one present can queue before it collects any. */
#define PENDING_MAX     (STRIP_COUNT * GRID_COLS + 2)

static int64_t pending_due_us[PENDING_MAX];
static int pending_head;
static int pending_count;
static int64_t bus_free_at_us;
static portMUX_TYPE pending_lock = portMUX_INITIALIZER_UNLOCKED;

static int clock_hz;
static void (*on_strip_done)(void);
static esp_timer_handle_t bus_timer;

static void
arm_bus_timer(int64_t due_us) {
    const int64_t wait_us = due_us - esp_timer_get_time();
    esp_timer_start_once(bus_timer, wait_us > 0 ? (uint64_t)wait_us : 1);
}

static void
strip_left_the_bus(void* arg) {
    (void)arg;
    int64_t next_due_us = 0;

    portENTER_CRITICAL(&pending_lock);
    pending_head = (pending_head + 1) % PENDING_MAX;
    pending_count--;
    if (pending_count > 0) {
        next_due_us = pending_due_us[pending_head];
    }
    portEXIT_CRITICAL(&pending_lock);

    on_strip_done();
    if (next_due_us != 0) {
        arm_bus_timer(next_due_us);
    }
}

static int64_t
transfer_us(int x0, int y0, int x1, int y1) {
    const int64_t bits = (int64_t)(x1 - x0) * (y1 - y0) * (int64_t)sizeof(gfx_color_t) * 8;
    return bits * 1000000 / ((int64_t)clock_hz * QSPI_DATA_LINES);
}

static esp_err_t
null_panel_draw_bitmap(esp_lcd_panel_t* self, int x0, int y0, int x1, int y1, const void* pixels) {
    (void)self;
    (void)pixels;
    const int64_t now_us = esp_timer_get_time();
    bool bus_was_idle = false;
    bool queued = false;
    int64_t due_us = 0;

    portENTER_CRITICAL(&pending_lock);
    if (pending_count < PENDING_MAX) {
        const int64_t start_us = bus_free_at_us > now_us ? bus_free_at_us : now_us;
        due_us = start_us + transfer_us(x0, y0, x1, y1);
        bus_free_at_us = due_us;
        pending_due_us[(pending_head + pending_count) % PENDING_MAX] = due_us;
        bus_was_idle = pending_count == 0;
        pending_count++;
        queued = true;
    }
    portEXIT_CRITICAL(&pending_lock);

    if (!queued) {
        on_strip_done();
    } else if (bus_was_idle) {
        arm_bus_timer(due_us);
    }
    return ESP_OK;
}

static esp_err_t
null_panel_del(esp_lcd_panel_t* self) {
    (void)self;
    return ESP_OK;
}

esp_err_t
gfx_null_panel_open(int hz, void (*strip_done)(void), esp_lcd_panel_handle_t* out_panel) {
    static esp_lcd_panel_t null_panel = {
        .draw_bitmap = null_panel_draw_bitmap,
        .del = null_panel_del,
    };

    clock_hz = hz;
    on_strip_done = strip_done;
    if (bus_timer == NULL) {
        const esp_timer_create_args_t args = {.callback = strip_left_the_bus, .name = "null_panel"};
        const esp_err_t err = esp_timer_create(&args, &bus_timer);
        if (err != ESP_OK) {
            return err;
        }
    }
    *out_panel = &null_panel;
    return ESP_OK;
}
