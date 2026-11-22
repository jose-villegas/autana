/* console_panelcap: development panel capture lifecycle and low-priority sender. */
#ifdef ESP_PLATFORM
#include "app/console/console_panelcap_device.h"

#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include "app/app_arena.h"
#include "console/console_frame_request.h"
#include "console/console_stream.h"
#include "core/memory.h"
#include "core/timing.h"
#include "gfx/present/capture/gfx_panel_capture.h"
#include "gfx/present/capture/panelcap.h"
#include "gfx/present/gfx_internal.h"
#include "gfx/present/gfx_present.h"

#define PANELCAP_ARGS_MAX     32U
#define PANELCAP_TAG          "PANEL"
#define PANELCAP_STOP_WAIT_MS 10U

bool gfx_panel_capture_on;
static panelcap_t capture;
static atomic_bool sender_done;
static uint64_t bytes_sent;
static bool launcher_visit;
static console_stream_t* stream;
static panelcap_encoder_t* encoder;

static void
redraw(const void* payload) {
    (void)payload;
    if (gfx_panel_capture_on) {
        gfx_present_drain();
        gfx_request_full_redraw();
        panelcap_redraw_requested(&capture);
    }
}

CONSOLE_FRAME_SIGNAL(console_panelcap_redraw_slot, redraw)

void
gfx_panel_capture_begin_active(void) {
    if (panelcap_begin(&capture, timing_now_us())) {
        (void)console_frame_post(console_frame_slots(), &console_panelcap_redraw_slot, NULL);
    }
}

void
gfx_panel_capture_commit_active(void) {
    panelcap_commit(&capture);
}

void
gfx_panel_capture_window(int x0, int y0, int x1, int y1, const gfx_color_t* pixels) {
    panelcap_window(&capture, x0, y0, x1, y1, pixels);
}

static size_t
fill(void* arg, uint8_t* packet, size_t capacity) {
    panelcap_t* source = arg;
    if (panelcap_peek(source) == NULL) {
        return 0;
    }
    size_t count = panelcap_fill(source, encoder, packet, capacity);
    if (count == 0) {
        *encoder = (panelcap_encoder_t){0};
        panelcap_release(source);
    }
    return count;
}

static void
ended(uint64_t sent, uint64_t packets, void* arg) {
    (void)arg;
    bytes_sent = sent - (packets * PANELCAP_PART_HEAD_BYTES);
    atomic_store_explicit(&sender_done, true, memory_order_release);
}

static void
stop(void) {
    gfx_present_drain();
    gfx_panel_capture_on = false;
    console_stream_close(stream, ended, NULL);
    /* The frame owner frees the app block after the sender's last read. */
    while (!atomic_load_explicit(&sender_done, memory_order_acquire)) {
        timing_sleep_ms(PANELCAP_STOP_WAIT_MS);
    }
    memory_free(encoder);
    char line[PANELCAP_LINE_BYTES];
    (void)snprintf(line, sizeof line, "PANELCAP_END presents=%lu dropped=%lu bytes=%llu",
                   (unsigned long)capture.presents, (unsigned long)capture.dropped, (unsigned long long)bytes_sent);
    console_emit_line_bounded(line);
}

void
gfx_panel_capture_stop(void) {
    if (!gfx_panel_capture_on) {
        console_emit_line_bounded("PANELCAP_REFUSED: not recording");
        return;
    }
    stop();
    gfx_panel_capture_leave_launcher();
}

void
gfx_panel_capture_end_visit(void) {
    if (gfx_panel_capture_on) {
        console_emit_line_bounded(PANELCAP_APP_SWITCH);
        stop();
    }
    launcher_visit = false;
}

void
gfx_panel_capture_leave_launcher(void) {
    if (launcher_visit) {
        app_arena_exit();
    }
}

void
gfx_panel_capture_start(unsigned fps, console_reply_fn reply) {
    if (gfx_panel_capture_on) {
        reply("PANELCAP_REFUSED: already on");
        return;
    }
    gfx_present_drain();
    launcher_visit = !app_heap_active();
    if (launcher_visit) {
        app_arena_enter();
    }
    char line[PANELCAP_LINE_BYTES];
    encoder = memory_alloc(PANELCAP_CAPTURE_BYTES, MEMORY_APP);
    if (encoder == NULL) {
        (void)snprintf(line, sizeof line, PANELCAP_NO_MEMORY, PANELCAP_CAPTURE_BYTES, memory_largest_block(MEMORY_APP));
        gfx_panel_capture_leave_launcher();
        reply(line);
        return;
    }
    uint8_t* image = (uint8_t*)(encoder + 1);
    uint8_t* packet = image + PANELCAP_IMAGE_BYTES;
    uint8_t* scratch = packet + CONSOLE_PACKET_BYTES_MAX;
    (void)panelcap_init(&capture, image, fps);
    *encoder = (panelcap_encoder_t){0};
    atomic_store_explicit(&sender_done, false, memory_order_relaxed);
    stream = console_stream_open_source(PANELCAP_TAG, fill, &capture, packet, scratch);
    if (stream == NULL) {
        memory_free(encoder);
        gfx_panel_capture_leave_launcher();
        reply("PANELCAP_REFUSED: no memory for sender");
        return;
    }
    gfx_panel_capture_on = true;
    gfx_request_full_redraw();
    (void)snprintf(line, sizeof line, "PANELCAP_BEGIN fps=%u image=%u", fps, (unsigned)PANELCAP_IMAGE_BYTES);
    reply(line);
}

static void
answer(const void* payload) {
    const unsigned fps = *(const unsigned*)payload;
    if (fps == 0) {
        gfx_panel_capture_stop();
    } else {
        gfx_panel_capture_start(fps, console_emit_line_bounded);
    }
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
