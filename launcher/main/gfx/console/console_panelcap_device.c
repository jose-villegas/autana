/* console_panelcap: development panel capture lifecycle and low-priority sender. */
#ifdef ESP_PLATFORM
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include "console/console.h"
#include "console/console_frame_request.h"
#include "console/console_stream.h"
#include "console/console_verbs.h"
#include "core/memory.h"
#include "core/timing.h"
#include "gfx/present/capture/gfx_panel_capture.h"
#include "gfx/present/capture/panelcap.h"
#include "gfx/present/gfx_internal.h"
#include "gfx/present/gfx_present.h"

#define PANELCAP_LINE_BYTES 128U
#define PANELCAP_ARGS_MAX   32U
#define PANELCAP_TAG        "PANEL"

bool gfx_panel_capture_on;
static panelcap_t capture;
static atomic_bool running;
static console_stream_t* stream;
static panelcap_encoder_t* encoder;
static uint64_t parts_sent;

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
    } else {
        parts_sent++;
    }
    return count;
}

static void
ended(uint64_t sent, void* arg) {
    panelcap_t* source = arg;
    memory_free(encoder);
    char line[PANELCAP_LINE_BYTES];
    (void)snprintf(line, sizeof line, "PANELCAP_END presents=%lu dropped=%lu bytes=%llu",
                   (unsigned long)source->presents, (unsigned long)source->dropped,
                   (unsigned long long)(sent - parts_sent * PANELCAP_PART_HEAD_BYTES));
    console_emit_line(line, "");
    atomic_store_explicit(&running, false, memory_order_release);
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
        console_stream_close(stream, ended, &capture);
        return;
    }
    if (atomic_load_explicit(&running, memory_order_acquire)) {
        console_emit_line("PANELCAP_REFUSED: already on", "");
        return;
    }
    encoder = memory_alloc(sizeof *encoder + PANELCAP_IMAGE_BYTES, MEMORY_PSRAM);
    if (encoder == NULL) {
        console_emit_line("PANELCAP_REFUSED: no PSRAM for panel image", "");
        return;
    }
    (void)panelcap_init(&capture, (uint8_t*)(encoder + 1), fps);
    parts_sent = 0;
    *encoder = (panelcap_encoder_t){0};
    stream = console_stream_open_source(PANELCAP_TAG, fill, &capture);
    if (stream == NULL) {
        memory_free(encoder);
        console_emit_line("PANELCAP_REFUSED: no memory for sender", "");
        return;
    }
    atomic_store_explicit(&running, true, memory_order_release);
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
