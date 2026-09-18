/*
 * screenshot - the device half: listens on the console for a trigger, then
 * prints the live framebuffer as base64 between marker lines that
 * tools/screenshot.py reads back out of the stream idf_monitor already uses.
 *
 * Also owns RUNSUITE (CONFIG_LAUNCHER_SELFTEST), which runs one named suite
 * instead of the whole boot-time run. It lives in this file because the
 * console has room for exactly one blocking reader - a second task reading
 * the same stream would race it for every byte.
 *
 * BOTH COMMANDS ONLY SET A FLAG. main.c's loop does the work, at a frame
 * boundary. There is no lock on the framebuffer, so a capture - or worse, a
 * suite that draws and presents on its own - running on this task while the
 * render loop runs on the main one is two tasks driving one panel.
 *
 * USB-Serial/JTAG, not UART: this board's USB-C is the S3's own peripheral
 * and the console's primary channel, so this listener sees the bytes
 * idf_monitor does. Its own task, because screenshot_start() switches the fd
 * to the driver's interrupt-driven reader, which is what lets a read block
 * instead of main.c polling every frame.
 */
#include "util/screenshot.h"

#include "build_id_generated.h"
#include "build_variant.h"
#include "util/build_id.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "driver/usb_serial_jtag.h"
#include "driver/usb_serial_jtag_vfs.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "gfx/gfx.h"
#include "util/device_state.h"

static const char* TAG = "screenshot";

static volatile bool s_request_pending;

/* A line over SCREENSHOT_LINE_MAX is never going to match either trigger
 * - dropped by resetting `len`, not by growing the buffer, so a host
 * accidentally in the wrong mode (pasting binary, say) cannot run this
 * off the end of a fixed buffer. Sized for RUNSUITE_TRIGGER plus the
 * longest suite name today (run_boot_anim_perf_suite, 25 chars) with
 * real headroom for names not yet written - bump this rather than trim
 * a name to fit it, the same "headroom, not a tight fit" reasoning
 * SUITE_MAX already states. */
#define SCREENSHOT_LINE_MAX 48

#if CONFIG_LAUNCHER_SELFTEST
/* Set by screenshot_task() below on a RUNSUITE line, consumed by
 * main.c's loop via screenshot_take_runsuite_request() - see this
 * file's own "WHY THE RESULT COMES BACK THROUGH A FLAG" section for
 * why this cannot just call suites_run_one() directly from this task.
 * Not stack-local for the same reason screenshot_dump()'s own scratch
 * buffer below is not: this outlives the line that set it, read back
 * by a different task entirely. */
static volatile bool s_runsuite_pending;
static char s_runsuite_name[SCREENSHOT_LINE_MAX];
#endif

static void
handle_screenshot_line(const char* line) {
    const build_console_command_t command = build_console_command_parse(line);
    if (command == BUILD_CONSOLE_SCREENSHOT) {
        ESP_LOGI(TAG, "trigger received");
        s_request_pending = true;
#if CONFIG_LAUNCHER_SELFTEST
    } else if (command == BUILD_CONSOLE_RUNSUITE) {
        const char* name = line + sizeof "RUNSUITE " - 1;
        ESP_LOGI(TAG, "RUNSUITE %s", name);
        strncpy(s_runsuite_name, name, sizeof(s_runsuite_name) - 1);
        s_runsuite_name[sizeof(s_runsuite_name) - 1] = '\0';
        s_runsuite_pending = true;
#endif
    } else if (command == BUILD_CONSOLE_BUILD_ID) {
        printf("BUILD_ID=%s\n", BUILD_ID);
        fflush(stdout);
    } else {
        ESP_LOGI(TAG, "ignoring line: '%s'", line);
    }
}

static void
screenshot_task(void* arg) {
    (void)arg;
    char line[SCREENSHOT_LINE_MAX];
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

        /* Either terminator ends a line - tools/screenshot.py sends a
         * bare '\n', but treating '\r' the same way means a line typed
         * by hand into monitor.sh (whose Enter key may send either,
         * depending on platform) still reaches the strcmp() below,
         * which is what makes "type SCREENSHOT into monitor.sh" a valid
         * way to test this listener in isolation from the host
         * script. */
        if (c == '\n' || c == '\r') {
            if (len > 0) {
                line[len] = '\0';
                handle_screenshot_line(line);
                len = 0;
            }
            continue;
        }

        if (len < SCREENSHOT_LINE_MAX - 1) {
            line[len++] = (char)c;
        } else {
            len = 0;
        }
    }
}

void
screenshot_start(void) {
    usb_serial_jtag_driver_config_t cfg = USB_SERIAL_JTAG_DRIVER_CONFIG_DEFAULT();
    const esp_err_t err = usb_serial_jtag_driver_install(&cfg);
    if (err != ESP_OK) {
        /* The one most worth calling out by name: this is what happens
         * if something else already installed this driver before
         * screenshot_start() ran (ESP_ERR_INVALID_STATE) - silently
         * leaving the console on its default non-blocking reader, which
         * looks from the host exactly like a request that vanished into
         * nothing rather than a boot-time failure. */
        ESP_LOGE(TAG, "usb_serial_jtag_driver_install failed: %s - listener not started", esp_err_to_name(err));
        return;
    }

    usb_serial_jtag_vfs_use_driver();

    /* Either terminator accepted on the way in - see screenshot_task()'s
     * own comment on why '\r' is treated the same as '\n' there. Left at
     * LF here (no translation) rather than switched to CR/CRLF:
     * translating would only rewrite '\r' into '\n' before
     * screenshot_task() ever sees it, which the task already does
     * itself, and leaving translation off means a stray '\r' from
     * either source arrives unchanged instead of being silently turned
     * into two line endings for one keypress. */
    usb_serial_jtag_vfs_set_rx_line_endings(ESP_LINE_ENDINGS_LF);

    /* Runs at a low priority (below touch/buttons - see input/touch.c,
     * input/buttons.c for their own 6/5) since it spends essentially
     * all its time blocked waiting on bytes nobody is usually sending;
     * when a line does arrive there is nothing time-critical about
     * noticing it a frame or two later. */
    const BaseType_t created = xTaskCreate(screenshot_task, "screenshot", 3072, NULL, 4, NULL);
    if (created != pdPASS) {
        ESP_LOGE(TAG, "xTaskCreate failed (out of memory?) - listener not started");
        return;
    }

#if CONFIG_LAUNCHER_SELFTEST
    ESP_LOGI(TAG, "listening for 'SCREENSHOT', 'BUILDID', and 'RUNSUITE <name>' on the console");
#else
    ESP_LOGI(TAG, "listening for 'SCREENSHOT' and 'BUILDID' on the console");
#endif
}

bool
screenshot_take_request(void) {
    if (!s_request_pending) {
        return false;
    }
    s_request_pending = false;
    return true;
}

#if CONFIG_LAUNCHER_SELFTEST
bool
screenshot_take_runsuite_request(char* name_out, size_t name_out_size) {
    if (!s_runsuite_pending) {
        return false;
    }
    s_runsuite_pending = false;
    strncpy(name_out, s_runsuite_name, name_out_size - 1);
    name_out[name_out_size - 1] = '\0';
    return true;
}
#endif

/* Not stack-local: screenshot_dump() runs on the shell task (3584-byte
 * stack), and a 1104-byte row plus 1472-byte base64 would be most of that
 * budget on top of printf/ESP_LOG's own use. Not permanently static either -
 * held only for the duration of a capture, because static here competes for
 * the largest contiguous heap block an app may need at runtime. */
static uint8_t* row;
static char* row_b64; /* +1: NUL, for printf("%s") */

/* How much room an app's diagnostic_json() fragment is given - see
 * app_t's own comment in app.h for what it may contain. Generous
 * relative to what any existing implementation actually uses, on the
 * same reasoning DEVICE_STATE_JSON_MAX budgets headroom rather than a
 * tight fit - this is a diagnostic path, not one worth re-deriving an
 * exact bound for. */
#define APP_DIAGNOSTIC_JSON_MAX 256

/* Protocol lines bypass stdio. The console VFS drops every byte once the
 * host has not drained the TX ring for 50 ms (TX_FLUSH_TIMEOUT_US in
 * usb_serial_jtag_vfs.c) - right for logs, fatal for a 660 KB capture,
 * which lost rows mid-stream on the S3. The driver call below waits
 * instead. */
/* No single write may exceed the driver's TX ring (tx_buffer_size in
 * screenshot_start()): a byte ringbuffer refuses a larger item outright,
 * however long it is told to wait. */
#define EMIT_CHUNK_BYTES        128

static void
emit_bytes(const char* bytes, size_t len) {
    while (len > 0) {
        const size_t n = len < EMIT_CHUNK_BYTES ? len : EMIT_CHUNK_BYTES;
        usb_serial_jtag_write_bytes(bytes, n, portMAX_DELAY);
        bytes += n;
        len -= n;
    }
}

static void
emit_line(const char* prefix, const char* payload) {
    emit_bytes(prefix, strlen(prefix));
    emit_bytes(payload, strlen(payload));
    emit_bytes("\n", 1);
}

/* Prints one SCREENSHOT_STATE: line of plain-text JSON (no base64 -
 * it's already printable ASCII, small enough that base64's reason to
 * exist, staying UART-safe, isn't worth the decode step for one line).
 * Reading/formatting live in util/device_state.h/.c. `current_app`'s
 * OPTIONAL diagnostic_json() is spliced in as an "app" key AFTER
 * device_state_format_json() produces a complete object - by
 * overwriting its closing `}` with `,"app":<fragment>}` rather than
 * teaching device_state.h about apps. */
static void
dump_state(const input_t* input, const app_t* current_app) {
    device_state_t state;
    device_state_read(&state);

    char json[DEVICE_STATE_JSON_MAX];
    device_state_format_json(&state, input, json);

    if (current_app != NULL && current_app->diagnostic_json != NULL) {
        char app_json[APP_DIAGNOSTIC_JSON_MAX];
        current_app->diagnostic_json(app_json, sizeof app_json);

        const size_t len = strlen(json);
        /* json[len-1] is device_state_format_json()'s own closing `}` -
         * always present, since that function always emits a complete
         * object. Only splice if there is genuinely room for the
         * fragment plus the `,"app":` wrapper plus the new closing `}`
         * - a truncated app fragment would rather be dropped than
         * emitted as broken JSON the host script's json.loads() then
         * rejects outright, losing the WHOLE line (device state
         * included, not just the app part) rather than only the
         * addition. */
        if (len > 0 && json[len - 1] == '}' && len - 1 + strlen(",\"app\":") + strlen(app_json) + 1 < sizeof json) {
            snprintf(json + len - 1, sizeof(json) - (len - 1), ",\"app\":%s}", app_json);
        }
    }

    emit_line("SCREENSHOT_STATE:", json);
}

void
screenshot_dump(const input_t* input, const app_t* current_app) {
    /* Band mode (gfx.h) has no framebuffer to read - gfx_framebuffer()
     * would hand back the internal-SRAM band ring's own NULL. Refuse with
     * a reason on the console rather than crash or stream garbage. */
    if (gfx_mode_current()->layout != GFX_LAYOUT_FULL_FB) {
        ESP_LOGW(TAG, "screenshot skipped - the running app has no framebuffer (band mode)");
        return;
    }

    const int32_t stride = screenshot_bmp_row_stride(GFX_WIDTH);
    const uint32_t pixel_bytes = (uint32_t)(stride * GFX_HEIGHT);
    const uint32_t total_bytes = SCREENSHOT_BMP_HEADER_SIZE + pixel_bytes;

    /* sizes named rather than re-derived from sizeof(row)/sizeof(row_b64)
     * below: row/row_b64 are pointers now (see their own declaration
     * comment), so sizeof on them would give the pointer's own size, not
     * the buffer's. */
    const size_t row_bytes = (size_t)GFX_WIDTH * 3;
    const size_t row_b64_bytes = (size_t)GFX_WIDTH * 4 + 1;

    row = malloc(row_bytes);
    row_b64 = malloc(row_b64_bytes);
    if (row == NULL || row_b64 == NULL) {
        ESP_LOGE(TAG,
                 "could not allocate %u+%u-byte row buffers - "
                 "screenshot skipped; largest free block is %u",
                 (unsigned)row_bytes, (unsigned)row_b64_bytes,
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
        free(row);
        free(row_b64);
        row = NULL;
        row_b64 = NULL;
        return;
    }

    ESP_LOGI(TAG, "streaming %lu bytes to the console", (unsigned long)total_bytes);

    /* The marker and data lines go out through emit_line(), not ESP_LOGx: a log
     * line carries a "I (12345) TAG: " prefix (see boot/post.c's
     * report() for an ordinary use of that prefix) that
     * tools/screenshot.py would otherwise have to strip back off before
     * the fixed-prefix match it does on every line - simpler for both
     * ends to keep the protocol's own lines free of it from the
     * start. */
    char begin[32];
    snprintf(begin, sizeof begin, "SCREENSHOT_BEGIN size=%lu", (unsigned long)total_bytes);
    fflush(stdout); /* buffered log output must not land inside the stream */
    emit_line(begin, "");

    uint8_t header[SCREENSHOT_BMP_HEADER_SIZE];
    screenshot_bmp_header(header, GFX_WIDTH, GFX_HEIGHT);
    /* 54 bytes -> 72 base64 chars (SCREENSHOT_BMP_HEADER_SIZE is a multiple
     * of 3, so this is exact with no padding - see screenshot_base64_encode()'s
     * own comment on why that property matters here). */
    char header_b64[72 + 1];
    screenshot_base64_encode(header, sizeof header, header_b64);
    header_b64[sizeof(header_b64) - 1] = '\0';
    emit_line("SCREENSHOT_DATA:", header_b64);

    const gfx_color_t* fb = gfx_framebuffer();

    /* Bottom-to-top, matching the bottom-up rows screenshot_bmp_header()
     * declares (positive biHeight) - see that function's own comment. */
    for (int32_t y = GFX_HEIGHT - 1; y >= 0; y--) {
        const gfx_color_t* src_row = fb + (size_t)y * GFX_WIDTH;
        for (int32_t x = 0; x < GFX_WIDTH; x++) {
            /* gfx_color_rgb888() is the panel-format-to-0xRRGGBB conversion
             * gfx_color.h already carries and tests (suite_gfx_color.c) -
             * reused rather than re-deriving the byte swap and channel
             * widths here. BMP's own pixel order is B, G, R. */
            const uint32_t rgb = gfx_color_rgb888(src_row[x]);
            row[x * 3 + 0] = (uint8_t)(rgb);
            row[x * 3 + 1] = (uint8_t)(rgb >> 8);
            row[x * 3 + 2] = (uint8_t)(rgb >> 16);
        }
        screenshot_base64_encode(row, row_bytes, row_b64);
        row_b64[row_b64_bytes - 1] = '\0';
        emit_line("SCREENSHOT_DATA:", row_b64);
    }

    dump_state(input, current_app);

    emit_line("SCREENSHOT_END", "");

    free(row);
    free(row_b64);
    row = NULL;
    row_b64 = NULL;
}
