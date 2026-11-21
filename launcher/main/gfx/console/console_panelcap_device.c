/* console_panelcap: development panel capture lifecycle and low-priority sender. */
#ifdef ESP_PLATFORM
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include "console/console.h"
#include "console/console_frame_request.h"
#include "console/console_verbs.h"
#include "core/memory.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "gfx/present/capture/gfx_panel_capture.h"
#include "gfx/present/capture/panelcap.h"
#include "gfx/present/gfx_internal.h"
#include "gfx/present/gfx_present.h"

#define PANELCAP_TASK_PRIORITY 1U
#define PANELCAP_TASK_STACK    3072U
#define PANELCAP_IDLE_MS       20U
#define PANELCAP_LINE_BYTES    128U
#define PANELCAP_ARGS_MAX      32U
#define PANELCAP_TAG           "PANEL"

bool gfx_panel_capture_on;
static panelcap_t capture;
static atomic_bool running;
static atomic_bool stopping;
static uint8_t* packet;
static uint8_t* scratch;
static uint64_t packed_sent;

void
gfx_panel_capture_begin(void) {
    if (gfx_panel_capture_on) {
        (void)panelcap_begin(&capture, esp_timer_get_time());
    }
}

void
gfx_panel_capture_commit(void) {
    if (gfx_panel_capture_on) {
        panelcap_commit(&capture);
    }
}

void
gfx_panel_capture_window(int x0, int y0, int x1, int y1, const gfx_color_t* pixels) {
    panelcap_window(&capture, x0, y0, x1, y1, pixels);
}

static void
send_packet(void* arg, const uint8_t* bytes, size_t count) {
    (void)arg;
    console_emit_packet(PANELCAP_TAG, bytes, count, scratch);
    packed_sent += count - PANELCAP_PART_HEAD_BYTES;
}

static void
sender(void* arg) {
    (void)arg;
    for (;;) {
        const uint8_t* bytes = panelcap_peek(&capture);
        if (bytes != NULL) {
            panelcap_send(&capture, packet, CONSOLE_PACKET_BYTES_MAX, send_packet, NULL);
            panelcap_release(&capture);
        } else if (atomic_load_explicit(&stopping, memory_order_acquire)) {
            break;
        } else {
            vTaskDelay(pdMS_TO_TICKS(PANELCAP_IDLE_MS));
        }
    }
    memory_free(capture.storage);
    memory_free(packet);
    memory_free(scratch);
    char line[PANELCAP_LINE_BYTES];
    (void)snprintf(line, sizeof line, "PANELCAP_END presents=%lu dropped=%lu bytes=%llu",
                   (unsigned long)capture.presents, (unsigned long)capture.dropped, (unsigned long long)packed_sent);
    console_emit_line(line, "");
    atomic_store_explicit(&running, false, memory_order_release);
    vTaskDelete(NULL);
}

static void
answer(const void* payload) {
    const unsigned fps = *(const unsigned*)payload;
    gfx_present_drain();
    if (fps == 0) {
        if (!gfx_panel_capture_on) {
            console_emit_line("PANELCAP_REFUSED: not recording", "");
            return;
        }
        gfx_panel_capture_on = false;
        atomic_store_explicit(&stopping, true, memory_order_release);
        return;
    }
    if (atomic_load_explicit(&running, memory_order_acquire)) {
        console_emit_line("PANELCAP_REFUSED: already on", "");
        return;
    }
    uint8_t* storage = memory_alloc(PANELCAP_IMAGE_BYTES, MEMORY_PSRAM);
    packet = memory_alloc(CONSOLE_PACKET_BYTES_MAX, MEMORY_PSRAM);
    scratch = memory_alloc(CONSOLE_PACKET_MAX, MEMORY_PSRAM);
    if (storage == NULL || packet == NULL || scratch == NULL) {
        memory_free(storage);
        memory_free(packet);
        memory_free(scratch);
        console_emit_line("PANELCAP_REFUSED: no PSRAM for panel image", "");
        return;
    }
    panelcap_init(&capture, storage, fps);
    packed_sent = 0;
    atomic_store(&stopping, false);
    atomic_store(&running, true);
    if (xTaskCreate(sender, "panelcap", PANELCAP_TASK_STACK, NULL, PANELCAP_TASK_PRIORITY, NULL) != pdPASS) {
        atomic_store(&running, false);
        memory_free(storage);
        memory_free(packet);
        memory_free(scratch);
        console_emit_line("PANELCAP_REFUSED: no memory for sender task", "");
        return;
    }
    gfx_panel_capture_on = true;
    gfx_request_full_redraw();
    char line[PANELCAP_LINE_BYTES];
    (void)snprintf(line, sizeof line, "PANELCAP_BEGIN fps=%u image=%u", fps, (unsigned)PANELCAP_IMAGE_BYTES);
    console_emit_line(line, "");
}

CONSOLE_FRAME_SLOT(console_panelcap_slot, unsigned, 0, answer)

static void
console_verb_panelcap(const char* args, console_reply_fn reply) {
    unsigned fps = PANELCAP_FPS_DEFAULT;
    if (strcasecmp(args, "stop") == 0) {
        fps = 0;
    } else if (strcasecmp(args, "start") != 0) {
        char* end;
        if (strncasecmp(args, "start ", sizeof "start " - 1) != 0) {
            reply("PANELCAP_REFUSED: usage PANELCAP START [fps] or PANELCAP STOP");
            return;
        }
        unsigned long parsed = strtoul(args + sizeof "start " - 1, &end, 10);
        if (*end != '\0' || parsed == 0 || parsed > PANELCAP_FPS_MAX) {
            reply("PANELCAP_REFUSED: bad fps");
            return;
        }
        fps = (unsigned)parsed;
    }
    (void)console_frame_post(console_frame_slots(), &console_panelcap_slot, &fps);
}

CONSOLE_VERB(panelcap, PANELCAP_ARGS_MAX, console_verb_panelcap)

#endif
