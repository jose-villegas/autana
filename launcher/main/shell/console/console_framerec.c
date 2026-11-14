/*
 * console_framerec (FRAMEREC START / FRAMEREC STOP): records each drawn
 * frame's render input (render/record/frame_record.h) into a ring in
 * external RAM, which a task below the frame loop sends as binary packets:
 *
 *   FRAMEREC_BIN <bytes> <crc32 hex>\n<bytes of the stream>
 *
 * Each packet goes out in one console write, so a log line from another
 * task lands between packets, never inside one. Frames go out as they come,
 * and a frame the ring has no room for is dropped whole and counted. While
 * FREEZE holds the loop, the frame a STEP lets past waits for room instead,
 * so a held frame arrives whole. Development only, like every console verb.
 */
#include "console/console.h"
#include "console/console_frame_request.h"
#include "console/console_verbs.h"
#include "core/memory.h"
#include "render/record/frame_record.h"
#include "shell/console/console_freeze.h"

#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/ringbuf.h"
#include "freertos/task.h"

#include <stdio.h>
#include <string.h>
#include <strings.h>

/* A few of the largest frames measured (the water tour's, about 20 KB): the
 * PSRAM outside the app's region has 225 to 245 KB in its largest block
 * beside a scene, and the recorder's own buffers come from there too. */
#define FRAMEREC_RING_BYTES    (96U * 1024U)
/* Below the console task (4), above the frame loop (1). */
#define FRAMEREC_TASK_PRIORITY 2
#define FRAMEREC_TASK_STACK    3072
#define FRAMEREC_DRAIN_WAIT_MS 20
/* How long a held frame waits for ring room before it is dropped after all:
 * long enough for a reader to drain the ring, short enough that a reader
 * gone away does not hold the loop. */
#define FRAMEREC_HELD_WAIT_MS  5000
#define FRAMEREC_ARGS_MAX      (sizeof "start" - 1)
#define FRAMEREC_TAG           "FRAMEREC"

/* One recording's: the drain task owns it from START until it ends itself. */
typedef struct {
    RingbufHandle_t ring;
    frame_record_counts_t counts; /* the recorder's, taken at STOP */
    uint64_t sent;
    volatile bool stopping; /* no more frames; the task drains and ends */
    uint8_t* packet;
} sink_t;

static sink_t sink;
/* Set from START until the drain task has sent FRAMEREC_END and ended. */
static TaskHandle_t volatile drain_task;

static bool
sink_write(void* state, const uint8_t* bytes, size_t count) {
    sink_t* s = state;
    if (s->stopping) {
        return false;
    }
    const TickType_t wait = console_freeze_held() ? pdMS_TO_TICKS(FRAMEREC_HELD_WAIT_MS) : 0;
    return xRingbufferSend(s->ring, bytes, count, wait) == pdTRUE;
}

static void
release_sink(void) {
    if (sink.ring != NULL) {
        vRingbufferDeleteWithCaps(sink.ring);
    }
    memory_free(sink.packet);
    sink = (sink_t){0};
}

/* Sends what the ring holds until STOP has emptied it, then the totals, and ends. */
static void
drain(void* arg) {
    (void)arg;
    for (;;) {
        size_t count = 0;
        uint8_t* bytes =
            xRingbufferReceiveUpTo(sink.ring, &count, pdMS_TO_TICKS(FRAMEREC_DRAIN_WAIT_MS), CONSOLE_PACKET_BYTES_MAX);
        if (bytes != NULL) {
            console_emit_packet(FRAMEREC_TAG, bytes, count, sink.packet);
            sink.sent += count;
            vRingbufferReturnItem(sink.ring, bytes);
        } else if (sink.stopping) {
            break;
        }
    }
    char line[96];
    (void)snprintf(line, sizeof line, "FRAMEREC_END frames=%lu dropped=%lu bytes=%llu",
                   (unsigned long)sink.counts.frames, (unsigned long)sink.counts.dropped,
                   (unsigned long long)sink.sent);
    console_emit_line(line, "");
    release_sink();
    drain_task = NULL;
    vTaskDelete(NULL);
}

static void
start(void) {
    if (drain_task != NULL) {
        console_emit_line("FRAMEREC_REFUSED: already recording", "");
        return;
    }
    sink = (sink_t){0};
    sink.ring = xRingbufferCreateWithCaps(FRAMEREC_RING_BYTES, RINGBUF_TYPE_BYTEBUF, MALLOC_CAP_SPIRAM);
    sink.packet = memory_alloc(CONSOLE_PACKET_MAX, MEMORY_PSRAM);
    TaskHandle_t task = NULL;
    if (sink.ring != NULL && sink.packet != NULL) {
        sink.stopping = !frame_record_start(&(frame_record_out_t){sink_write, &sink}, FRAME_RECORD_KEYFRAME_EVERY);
        if (sink.stopping) {
            console_emit_line("FRAMEREC_REFUSED: the recorder has no memory", "");
            release_sink();
            return;
        }
        (void)xTaskCreate(drain, "framerec", FRAMEREC_TASK_STACK, NULL, FRAMEREC_TASK_PRIORITY, &task);
    }
    if (task == NULL) {
        char why[96];
        (void)snprintf(why, sizeof why, "FRAMEREC_REFUSED: no memory: ring=%d packet=%d task=%d psram largest=%u",
                       sink.ring != NULL, sink.packet != NULL, sink.ring != NULL && sink.packet != NULL,
                       (unsigned)memory_largest_block(MEMORY_PSRAM));
        console_emit_line(why, "");
        frame_record_stop();
        release_sink();
        return;
    }
    drain_task = task;
    char line[64];
    (void)snprintf(line, sizeof line, "FRAMEREC_BEGIN ring=%u keyframe_every=%d", (unsigned)FRAMEREC_RING_BYTES,
                   FRAME_RECORD_KEYFRAME_EVERY);
    console_emit_line(line, "");
}

static void
answer(const void* payload) {
    const bool starting = *(const bool*)payload;
    if (starting) {
        start();
    } else if (drain_task != NULL && !sink.stopping) {
        sink.counts = frame_record_counts();
        frame_record_stop();
        sink.stopping = true;
    } else {
        console_emit_line("FRAMEREC_REFUSED: not recording", "");
    }
}

CONSOLE_FRAME_SLOT(console_framerec_slot, bool, false, answer)

static void
console_verb_framerec(const char* args, console_reply_fn reply) {
    bool starting;
    if (strcasecmp(args, "start") == 0) {
        starting = true;
    } else if (strcasecmp(args, "stop") == 0) {
        starting = false;
    } else {
        reply("FRAMEREC_REFUSED: usage FRAMEREC START or FRAMEREC STOP");
        return;
    }
    (void)console_frame_post(console_frame_slots(), &console_framerec_slot, &starting);
}

CONSOLE_VERB(framerec, FRAMEREC_ARGS_MAX, console_verb_framerec)
