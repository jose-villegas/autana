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
#include "core/job.h"
#include "core/memory.h"
#include "core/timing.h"
#include "esp_memory_utils.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "gfx/present/capture/gfx_panel_capture.h"
#include "gfx/present/capture/panelcap.h"
#include "gfx/present/gfx_internal.h"
#include "gfx/present/gfx_present.h"

#define PANELCAP_ARGS_MAX      32U
#define PANELCAP_STOP_WAIT_MS  10U
/* Below the present and compute tasks on their core. */
#define PANELCAP_TASK_PRIORITY 1U

typedef enum {
    PROBE_FULL,
    PROBE_COPY,
    PROBE_INTERNAL,
    PROBE_READ,
    PROBE_LOW,
} probe_mode_t;

#define PROBE_ROW_BYTES     (PANELCAP_WIDTH * PANELCAP_PIXEL_BYTES)
#define PROBE_COPY_PRIORITY (JOB_CORE1_PRIORITY - 1)

static const char* const probe_modes[] = {"full", "copy", "internal", "read", "low"};
static probe_mode_t probe_mode;
static uint8_t* probe_scratch;
static bool probe_logged;

bool console_panelcap_on;
static panelcap_t capture;
static atomic_bool sender_done;
static bool launcher_visit;
static bool redraw_full;
static console_stream_t* stream;
static panelcap_encoder_t* encoder;

static struct {
    uint64_t bytes;
    uint64_t encode_us;
    uint64_t write_us;
    uint32_t sent;
    uint32_t failed;
    bool last_part;
} sender;

static struct {
    uint64_t window_us;
    uint64_t redraw_pixels;
    uint32_t redraws;
} present;

static void
redraw(const void* payload) {
    (void)payload;
    if (console_panelcap_on) {
        gfx_present_drain();
        const recti_t box = panelcap_redraw_requested(&capture);
        const bool force_full = redraw_full;
        redraw_full = false;
        if (recti_is_empty(box)) {
            return;
        }
        if (force_full || !gfx_mode_keeps_picture(gfx_mode_current())) {
            present.redraw_pixels += PANELCAP_WIDTH * PANELCAP_HEIGHT;
            gfx_request_full_redraw();
        } else {
            present.redraw_pixels += recti_area(box);
            gfx_mark_dirty(box.x, box.y, box.w, box.h);
        }
        present.redraws++;
    }
}

CONSOLE_FRAME_SIGNAL(console_panelcap_redraw_slot, redraw)

void
console_panelcap_begin_active(void) {
    if (panelcap_begin(&capture, timing_now_us())) {
        (void)console_frame_post(console_frame_slots(), &console_panelcap_redraw_slot, NULL);
    }
}

void
console_panelcap_commit_active(void) {
    redraw_full |= !recti_is_empty(capture.owed) && gfx_frame_expanded();
    const uint32_t previous = capture.presents;
    panelcap_commit(&capture);
    if (capture.presents != previous) {
        if (stream == NULL) {
            panelcap_release(&capture, true);
            sender.sent++;
        } else {
            console_stream_wake(stream);
        }
    }
}

static void
probe_copy_internal(uint8_t* dst, const uint8_t* src, size_t bytes) {
    (void)dst;
    memcpy(probe_scratch, src, bytes);
}

static void
probe_read_psram(uint8_t* dst, const uint8_t* src, size_t bytes) {
    (void)src;
    const volatile uint16_t* words = (const volatile uint16_t*)dst;
    for (size_t i = 0; i < bytes / sizeof *words; i++) {
        (void)words[i];
    }
}

void
console_panelcap_window(int x0, int y0, int x1, int y1, const gfx_color_t* pixels) {
    const int64_t started = timing_now_us();
    const TaskHandle_t task = xTaskGetCurrentTaskHandle();
    const UBaseType_t priority = uxTaskPriorityGet(task);
    if (!probe_logged) {
        printf("PANELCAP_CALLER task=%s core=%d priority=%u src_psram=%d dst_psram=%d\n", pcTaskGetName(task),
               xPortGetCoreID(), (unsigned)priority, esp_ptr_external_ram(pixels),
               esp_ptr_external_ram(capture.storage));
        probe_logged = true;
    }
    const bool lower = probe_mode == PROBE_LOW && priority > PROBE_COPY_PRIORITY;
    if (lower) {
        vTaskPrioritySet(task, PROBE_COPY_PRIORITY);
    }
    panelcap_window(&capture, x0, y0, x1, y1, pixels);
    if (lower) {
        vTaskPrioritySet(task, priority);
    }
    present.window_us += (uint64_t)(timing_now_us() - started);
}

static size_t
fill(void* arg, uint8_t* packet, size_t capacity, uint32_t write_us, bool sent) {
    panelcap_t* source = arg;
    if (encoder->part != 0) {
        sender.write_us += write_us;
        if (sender.last_part || !sent) {
            sender.sent += sent;
            sender.failed += !sent;
            *encoder = (panelcap_encoder_t){0};
            panelcap_release(source, sent);
            return 0;
        }
    }
    if (panelcap_peek(source) == NULL) {
        return 0;
    }
    const int64_t started = timing_now_us();
    const size_t count = panelcap_fill(source, encoder, packet, capacity);
    sender.encode_us += (uint64_t)(timing_now_us() - started);
    sender.last_part = packet[PANELCAP_PART_AT_LAST] != 0;
    return count;
}

static void
ended(uint64_t sent, uint64_t packets, void* arg) {
    (void)arg;
    sender.bytes = sent - (packets * PANELCAP_PART_HEAD_BYTES);
    atomic_store_explicit(&sender_done, true, memory_order_release);
}

static void
stop(void) {
    gfx_present_drain();
    const int64_t stop_us = timing_now_us();
    console_panelcap_on = false;
    if (stream != NULL) {
        console_stream_close(stream, ended, NULL);
    }
    /* The frame owner frees the app block after the sender's last read. */
    while (stream != NULL && !atomic_load_explicit(&sender_done, memory_order_acquire)) {
        timing_sleep_ms(PANELCAP_STOP_WAIT_MS);
    }
    memory_free(encoder);
    encoder = NULL;
    if (probe_scratch != NULL) {
        memory_free(probe_scratch);
        probe_scratch = NULL;
    }
    stream = NULL;
    char line[PANELCAP_LINE_BYTES];
    (void)snprintf(line, sizeof line, "PANELCAP_STATS encode_us=%llu write_us=%llu link_failed=%lu",
                   (unsigned long long)sender.encode_us, (unsigned long long)sender.write_us,
                   (unsigned long)sender.failed);
    console_emit_line_bounded(line);
    (void)snprintf(line, sizeof line, "PANELCAP_STATS fps_gate=%lu image_busy=%lu redraw_wait=%lu",
                   (unsigned long)capture.dropped_fps, (unsigned long)capture.dropped_busy,
                   (unsigned long)capture.dropped_redraw);
    console_emit_line_bounded(line);
    (void)snprintf(line, sizeof line, "PANELCAP_STATS window_us=%llu copy_bytes=%llu redraws=%lu redraw_pixels=%llu",
                   (unsigned long long)present.window_us, (unsigned long long)capture.copied_bytes,
                   (unsigned long)present.redraws, (unsigned long long)present.redraw_pixels);
    console_emit_line_bounded(line);
    (void)snprintf(line, sizeof line, "PANELCAP_END presents=%lu dropped=%lu bytes=%llu stop_us=%lld",
                   (unsigned long)sender.sent, (unsigned long)capture.dropped, (unsigned long long)sender.bytes,
                   (long long)stop_us);
    console_emit_line_bounded(line);
}

void
console_panelcap_stop(void) {
    if (!console_panelcap_on) {
        console_emit_line_bounded("PANELCAP_REFUSED: not recording");
        return;
    }
    stop();
    console_panelcap_leave_launcher();
}

void
console_panelcap_end_visit(void) {
    if (console_panelcap_on) {
        console_emit_line_bounded(PANELCAP_APP_SWITCH);
        stop();
    }
    launcher_visit = false;
}

void
console_panelcap_leave_launcher(void) {
    if (launcher_visit) {
        console_panelcap_end_visit();
        app_arena_exit();
    }
}

void
console_panelcap_start(unsigned fps, console_reply_fn reply) {
    if (console_panelcap_on) {
        reply("PANELCAP_REFUSED: already on");
        return;
    }
    gfx_present_drain();
    launcher_visit = !app_heap_active() && app_arena_bytes() != 0;
    if (launcher_visit) {
        app_arena_enter();
    }
    char line[PANELCAP_LINE_BYTES];
    const bool heap_ready = app_heap_active();
    encoder = heap_ready ? memory_alloc(PANELCAP_CAPTURE_BYTES, MEMORY_APP) : NULL;
    if (encoder == NULL) {
        const size_t largest = heap_ready ? memory_largest_block(MEMORY_APP) : 0;
        (void)snprintf(line, sizeof line, PANELCAP_NO_MEMORY, PANELCAP_CAPTURE_BYTES, largest);
        console_panelcap_leave_launcher();
        reply(line);
        return;
    }
    uint8_t* image = (uint8_t*)(encoder + 1);
    uint8_t* packet = image + PANELCAP_IMAGE_BYTES;
    uint8_t* scratch = packet + CONSOLE_PACKET_BYTES_MAX;
    (void)panelcap_init(&capture, image, fps);
    redraw_full = false;
    memset(&sender, 0, sizeof sender);
    memset(&present, 0, sizeof present);
    *encoder = (panelcap_encoder_t){0};
    atomic_store_explicit(&sender_done, false, memory_order_relaxed);
    const int core = configNUMBER_OF_CORES > JOB_CORE1 ? JOB_CORE1 : tskNO_AFFINITY;
    probe_logged = false;
    if (probe_mode == PROBE_INTERNAL) {
        probe_scratch = memory_alloc(PROBE_ROW_BYTES, MEMORY_INTERNAL);
        if (probe_scratch == NULL) {
            memory_free(encoder);
            encoder = NULL;
            console_panelcap_leave_launcher();
            reply("PANELCAP_REFUSED: no internal probe memory");
            return;
        }
        capture.probe_copy = probe_copy_internal;
    } else if (probe_mode == PROBE_READ) {
        capture.probe_copy = probe_read_psram;
    }
    if (probe_mode == PROBE_FULL || probe_mode == PROBE_LOW) {
        stream = console_stream_open_source(PANELCAP_PACKET_TAG, fill, &capture, packet, scratch,
                                            PANELCAP_TASK_PRIORITY, core);
        if (stream == NULL) {
            memory_free(encoder);
            encoder = NULL;
            console_panelcap_leave_launcher();
            reply(PANELCAP_NO_SENDER);
            return;
        }
    }
    console_panelcap_on = true;
    gfx_request_full_redraw();
    (void)snprintf(line, sizeof line, "PANELCAP_BEGIN fps=%u image=%u", fps, (unsigned)PANELCAP_IMAGE_BYTES);
    reply(line);
}

typedef struct {
    unsigned fps;
    probe_mode_t mode;
} probe_request_t;

static void
answer(const void* payload) {
    const probe_request_t* request = payload;
    if (request->fps == 0) {
        console_panelcap_stop();
    } else {
        if (!console_panelcap_on) {
            probe_mode = request->mode;
        }
        console_panelcap_start(request->fps, console_emit_line_bounded);
    }
}

CONSOLE_FRAME_SLOT(console_panelcap_slot, probe_request_t, 0, answer)

static void
console_verb_panelcap(const char* args, console_reply_fn reply) {
    probe_request_t request = {.fps = PANELCAP_FPS_DEFAULT};
    if (strcasecmp(args, "stop") == 0) {
        request.fps = 0;
    } else if (strcasecmp(args, "start") != 0) {
        char* end;
        if (strncasecmp(args, "start ", sizeof "start " - 1) != 0) {
            reply("PANELCAP_REFUSED: usage PANELCAP START [fps] or PANELCAP STOP");
            return;
        }
        unsigned long parsed = strtoul(args + sizeof "start " - 1, &end, 10);
        if ((*end != '\0' && *end != ' ') || parsed == 0 || parsed > PANELCAP_FPS_MAX) {
            reply("PANELCAP_REFUSED: bad fps");
            return;
        }
        request.fps = (unsigned)parsed;
        if (*end == ' ') {
            size_t i = 0;
            for (; i < sizeof probe_modes / sizeof probe_modes[0]; i++) {
                if (strcmp(end + 1, probe_modes[i]) == 0) {
                    break;
                }
            }
            if (i == sizeof probe_modes / sizeof probe_modes[0]) {
                reply("PANELCAP_REFUSED: bad probe mode");
                return;
            }
            request.mode = (probe_mode_t)i;
        }
    }
    (void)console_frame_post(console_frame_slots(), &console_panelcap_slot, &request);
}

CONSOLE_VERB(panelcap, PANELCAP_ARGS_MAX, console_verb_panelcap)

#endif
