/*
 * console - see console.h. The device half: installs whichever serial
 * driver this build's console runs on, then blocks a dedicated task on it
 * for one verb line at a time, matched against console_shared()'s
 * registry. A line nothing there claims is latched instead of logged -
 * see console_app_line.h for what the frame loop does with it.
 *
 * Every verb this dispatches to only sets a flag or writes a small reply -
 * none of them draw, none call into gfx or an app. That split matters most
 * for SCREENSHOT and RUNSUITE (console_screenshot.c, console_runsuite.c):
 * there is no lock on the framebuffer, so a capture - or worse, a suite
 * that draws and presents on its own - running on this task while the
 * render loop runs on the main one would be two tasks driving one panel.
 */
#include "console/console.h"
#include "console/console_app_line.h"
#include "console/console_latch.h"

#include <stdio.h>
#include <string.h>

#if CONFIG_LAUNCHER_QEMU
#include "driver/uart.h"
#include "driver/uart_vfs.h"
#else
#include "driver/usb_serial_jtag.h"
#include "driver/usb_serial_jtag_vfs.h"
#endif
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char* TAG = "console";

static console_registry_t shared;

console_registry_t*
console_shared(void) {
    return &shared;
}

/* A line no registered verb claimed - see console_app_line.h's own comment
 * for why this task only ever latches one, never decides whether an app
 * wants it. */
static console_latch_t app_line_latch;

bool
console_take_unclaimed_line(char* out, size_t out_size) {
    return console_latch_take(&app_line_latch, out, out_size);
}

void
console_reply_stdio(const char* line) {
    printf("%s\n", line);
    fflush(stdout);
}

/* Line endings stay LF, untranslated: console_task() accepts either
 * terminator itself, and translating would turn one keypress's '\r' into
 * two line endings. */
#if CONFIG_LAUNCHER_QEMU
#define CONSOLE_UART        ((uart_port_t)CONFIG_ESP_CONSOLE_UART_NUM)
#define CONSOLE_UART_RX_BUF 256

static esp_err_t
console_driver_install(void) {
    const esp_err_t err = uart_driver_install(CONSOLE_UART, CONSOLE_UART_RX_BUF, 0, 0, NULL, 0);
    if (err == ESP_OK) {
        uart_vfs_dev_use_driver(CONSOLE_UART);
        uart_vfs_dev_port_set_rx_line_endings(CONSOLE_UART, ESP_LINE_ENDINGS_LF);
    }
    return err;
}

static void
console_write(const char* bytes, size_t len) {
    uart_write_bytes(CONSOLE_UART, bytes, len);
}
#else
static esp_err_t
console_driver_install(void) {
    usb_serial_jtag_driver_config_t cfg = USB_SERIAL_JTAG_DRIVER_CONFIG_DEFAULT();
    const esp_err_t err = usb_serial_jtag_driver_install(&cfg);
    if (err == ESP_OK) {
        usb_serial_jtag_vfs_use_driver();
        usb_serial_jtag_vfs_set_rx_line_endings(ESP_LINE_ENDINGS_LF);
    }
    return err;
}

static void
console_write(const char* bytes, size_t len) {
    usb_serial_jtag_write_bytes(bytes, len, portMAX_DELAY);
}
#endif

/* Protocol lines bypass stdio. The console VFS drops every byte once the
 * host has not drained the TX ring for 50 ms (TX_FLUSH_TIMEOUT_US in
 * usb_serial_jtag_vfs.c) - right for logs, fatal for a 660 KB capture,
 * which lost rows mid-stream on the S3. The driver call below waits
 * instead. */
/* No single write may exceed the driver's TX ring (tx_buffer_size in
 * console_start()): a byte ringbuffer refuses a larger item outright,
 * however long it is told to wait. */
#define EMIT_CHUNK_BYTES 128

static void
emit_bytes(const char* bytes, size_t len) {
    while (len > 0) {
        const size_t n = len < EMIT_CHUNK_BYTES ? len : EMIT_CHUNK_BYTES;
        console_write(bytes, n);
        bytes += n;
        len -= n;
    }
}

void
console_emit_line(const char* prefix, const char* payload) {
    emit_bytes(prefix, strlen(prefix));
    emit_bytes(payload, strlen(payload));
    emit_bytes("\n", 1);
}

/* Appends `c` to `line` (tracked by `*len`), returning true once it
 * completes one. */
static bool
console_append_char(char* line, int* len, int c) {
    /* Either terminator ends a line: monitor.sh's Enter key may send '\r'
     * or '\n', depending on platform. */
    if (c == '\n' || c == '\r') {
        if (*len == 0) {
            return false;
        }
        line[*len] = '\0';
        *len = 0;
        return true;
    }
    /* Dropped by resetting `*len`, not by growing the buffer: a line over
     * CONSOLE_LINE_MAX cannot match any verb regardless. */
    if (*len < CONSOLE_LINE_MAX - 1) {
        line[(*len)++] = (char)c;
    } else {
        *len = 0;
    }
    return false;
}

static void
console_task(void* arg) {
    (void)arg;
    char line[CONSOLE_LINE_MAX];
    int len = 0;

    while (1) {
        const int c = fgetc(stdin);
        if (c == EOF) {
            /* Should not happen once the driver is installed (fgetc blocks
             * until a byte arrives) - guarded anyway so a console detached
             * mid-run degrades to a slow poll instead of a spin loop. */
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }
        if (console_append_char(line, &len, c)) {
            console_dispatch_or_latch(&shared, line, console_reply_stdio, &app_line_latch);
        }
    }
}

/* Names what registered, so a verb this build does not link is not claimed.
 * screenshot goes first whatever its place in name order: a host script
 * knows the console is up by the text "listening for 'screenshot'"
 * (launcher/test/qemu_run.py). */
static void
log_listening(void) {
    char line[192];
    int n = snprintf(line, sizeof line, "listening for 'screenshot'");
    for (const console_verb_t* entry = shared.first; entry != NULL && n < (int)sizeof line; entry = entry->next) {
        if (strcmp(entry->name, "screenshot") == 0) {
            continue;
        }
        n += snprintf(line + n, sizeof(line) - (size_t)n, ", '%s'", entry->name);
    }
    ESP_LOGI(TAG, "%s on the console", line);
}

void
console_start(void) {
    const esp_err_t err = console_driver_install();
    if (err != ESP_OK) {
        /* The one most worth calling out by name: this is what happens if
         * something else already installed this driver before
         * console_start() ran (ESP_ERR_INVALID_STATE) - silently leaving
         * the console on its default non-blocking reader, which looks from
         * the host exactly like a request that vanished into nothing
         * rather than a boot-time failure. */
        ESP_LOGE(TAG, "console driver install failed: %s - listener not started", esp_err_to_name(err));
        return;
    }

    /* Runs at a low priority (below touch/buttons - see input/touch.c,
     * input/buttons.c for their own 6/5) since it spends essentially all
     * its time blocked waiting on bytes nobody is usually sending; when a
     * line does arrive there is nothing time-critical about noticing it a
     * frame or two later. */
    const BaseType_t created = xTaskCreate(console_task, "console", 3072, NULL, 4, NULL);
    if (created != pdPASS) {
        ESP_LOGE(TAG, "xTaskCreate failed (out of memory?) - listener not started");
        return;
    }

    log_listening();
}
