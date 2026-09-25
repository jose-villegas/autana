/*
 * console_screenshot - SCREENSHOT: prints the frame gfx holds
 * (gfx_read_panel_row()) as base64 between marker lines that
 * tools/device/screenshot.py reads back out of the console stream idf_monitor
 * already uses. The verb itself only sets a latch; console_screenshot_dump()
 * does the actual streaming, called from main.c's frame loop - see
 * console.c's own top comment for why nothing here may draw on this task.
 * Development builds only - see console.h.
 */
#include "console/console_screenshot.h"
#include "console/console.h"
#include "console/console_latch.h"
#include "console/console_verbs.h"

#include "console/device_state.h"
#include "util/screenshot.h"

#include "esp_heap_caps.h"
#include "esp_log.h"

#include "gfx/gfx.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char* TAG = "screenshot";

static console_latch_t screenshot_latch;

static void
console_verb_screenshot(const char* args, console_reply_fn reply) {
    (void)args;
    (void)reply;
    ESP_LOGI(TAG, "trigger received");
    console_latch_set(&screenshot_latch, "");
}

CONSOLE_VERB(screenshot, 0, console_verb_screenshot)

bool
console_screenshot_take_request(void) {
    char unused[1];
    return console_latch_take(&screenshot_latch, unused, sizeof unused);
}

/* Not stack-local: console_screenshot_dump() runs on the shell task
 * (3584-byte stack), and a 736-byte pixel row, its 1104-byte BMP row and
 * 1472 bytes of base64 would be most of that budget on top of
 * printf/ESP_LOGI's own use. Not permanently static either - held only for
 * the duration of a capture, because static here competes for the largest
 * contiguous heap block an app may need at runtime. */
static gfx_color_t* pixels;
static uint8_t* row;
static char* row_b64; /* +1: NUL, for printf("%s") */

/* How much room an app's diagnostic_json() fragment is given - see app_t's
 * own comment in app.h for what it may contain. Generous relative to what
 * any existing implementation actually uses, on the same reasoning
 * DEVICE_STATE_JSON_MAX budgets headroom rather than a tight fit - this is
 * a diagnostic path, not one worth re-deriving an exact bound for. */
#define APP_DIAGNOSTIC_JSON_MAX 256

/* Prints one SCREENSHOT_STATE: line of plain-text JSON (no base64 - it's
 * already printable ASCII, small enough that base64's reason to exist,
 * staying UART-safe, isn't worth the decode step for one line). Reading/
 * formatting live in console/device_state.h/.c. `current_app`'s OPTIONAL
 * diagnostic_json() is spliced in as an "app" key AFTER
 * device_state_format_json() produces a complete object - by overwriting
 * its closing `}` with `,"app":<fragment>}` rather than teaching
 * device_state.h about apps. */
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
         * object. Only splice if there is genuinely room for the fragment
         * plus the `,"app":` wrapper plus the new closing `}` - a
         * truncated app fragment would rather be dropped than emitted as
         * broken JSON the host script's json.loads() then rejects
         * outright, losing the WHOLE line (device state included, not
         * just the app part) rather than only the addition. */
        if (len > 0 && json[len - 1] == '}' && len - 1 + strlen(",\"app\":") + strlen(app_json) + 1 < sizeof json) {
            snprintf(json + len - 1, sizeof(json) - (len - 1), ",\"app\":%s}", app_json);
        }
    }

    console_emit_line("SCREENSHOT_STATE:", json);
}

static void
end_capture(void) {
    gfx_readback_end();
    free(pixels);
    free(row);
    free(row_b64);
    pixels = NULL;
    row = NULL;
    row_b64 = NULL;
}

/* Sent in place of the whole capture, so the host stops waiting at once. */
static void
refuse(const char* reason) {
    ESP_LOGW(TAG, "screenshot refused - %s", reason);
    fflush(stdout);
    console_emit_line("SCREENSHOT_REFUSED:", reason);
    end_capture();
}

/* How many frames a band-mode capture waits for the app to draw a whole
 * frame before giving up - an app that stops calling gfx_band_next(). */
#define READBACK_PENDING_FRAMES_MAX 60

static int s_readback_pending_frames;

/* True once the frame is readable. Otherwise asks again next frame, or
 * refuses once that has gone on too long. */
static bool
readback_ready(void) {
    switch (gfx_readback_begin()) {
        case GFX_READBACK_READY: s_readback_pending_frames = 0; return true;
        case GFX_READBACK_UNAVAILABLE:
            refuse("band mode, and no room in PSRAM for a snapshot of the frame");
            return false;
        case GFX_READBACK_PENDING: break;
    }
    if (++s_readback_pending_frames > READBACK_PENDING_FRAMES_MAX) {
        s_readback_pending_frames = 0;
        refuse("band mode, and the app drew no complete frame to copy");
        return false;
    }
    console_latch_set(&screenshot_latch, "");
    return false;
}

static bool
alloc_row_buffers(size_t pixels_bytes, size_t row_bytes, size_t row_b64_bytes) {
    pixels = malloc(pixels_bytes);
    row = malloc(row_bytes);
    row_b64 = malloc(row_b64_bytes);
    if (pixels != NULL && row != NULL && row_b64 != NULL) {
        return true;
    }
    char reason[96];
    snprintf(reason, sizeof reason, "could not allocate %u bytes of row buffers; largest free block is %u",
             (unsigned)(pixels_bytes + row_bytes + row_b64_bytes),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
    refuse(reason);
    return false;
}

/* Marker and data lines go through console_emit_line(), not ESP_LOGx, so
 * tools/device/screenshot.py's fixed-prefix match never has to strip a log
 * prefix off first. Flushed before the first one, since buffered log
 * output must not land inside the stream. */
static void
emit_begin_and_header(uint32_t total_bytes) {
    char begin[32];
    snprintf(begin, sizeof begin, "SCREENSHOT_BEGIN size=%lu", (unsigned long)total_bytes);
    fflush(stdout);
    console_emit_line(begin, "");

    uint8_t header[SCREENSHOT_BMP_HEADER_SIZE];
    screenshot_bmp_header(header, GFX_WIDTH, GFX_HEIGHT);
    char header_b64[72 + 1]; /* 54 bytes -> 72 base64 chars, exact, no '=' padding */
    screenshot_base64_encode(header, sizeof header, header_b64);
    header_b64[sizeof(header_b64) - 1] = '\0';
    console_emit_line("SCREENSHOT_DATA:", header_b64);
}

/* Bottom-to-top, matching the bottom-up rows screenshot_bmp_header()
 * declares (positive biHeight). BMP's own pixel order is B, G, R, so each
 * pixel goes through gfx_color_rgb888() (gfx_color.h, tested by
 * suite_gfx_color.c) rather than re-deriving the byte swap here. */
static void
emit_rows(size_t row_bytes, size_t row_b64_bytes) {
    for (int32_t y = GFX_HEIGHT - 1; y >= 0; y--) {
        gfx_read_panel_row(y, pixels);
        for (int32_t x = 0; x < GFX_WIDTH; x++) {
            const uint32_t rgb = gfx_color_rgb888(pixels[x]);
            row[x * 3 + 0] = (uint8_t)(rgb);
            row[x * 3 + 1] = (uint8_t)(rgb >> 8);
            row[x * 3 + 2] = (uint8_t)(rgb >> 16);
        }
        screenshot_base64_encode(row, row_bytes, row_b64);
        row_b64[row_b64_bytes - 1] = '\0';
        console_emit_line("SCREENSHOT_DATA:", row_b64);
    }
}

void
console_screenshot_dump(const input_t* input, const app_t* current_app) {
    if (!readback_ready()) {
        return;
    }

    const int32_t stride = screenshot_bmp_row_stride(GFX_WIDTH);
    const uint32_t pixel_bytes = (uint32_t)(stride * GFX_HEIGHT);
    const uint32_t total_bytes = SCREENSHOT_BMP_HEADER_SIZE + pixel_bytes;

    const size_t pixels_bytes = (size_t)GFX_WIDTH * sizeof(gfx_color_t);
    const size_t row_bytes = (size_t)GFX_WIDTH * 3;
    const size_t row_b64_bytes = (size_t)GFX_WIDTH * 4 + 1;

    if (!alloc_row_buffers(pixels_bytes, row_bytes, row_b64_bytes)) {
        return;
    }

    ESP_LOGI(TAG, "streaming %lu bytes to the console", (unsigned long)total_bytes);
    emit_begin_and_header(total_bytes);
    emit_rows(row_bytes, row_b64_bytes);
    dump_state(input, current_app);
    console_emit_line("SCREENSHOT_END", "");
    end_capture();
}
