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
#include "hash/crc32.h"

#include <stdio.h>
#include <string.h>
#include <strings.h>

/* A few of the largest frames measured (the water tour's, about 20 KB): the
 * PSRAM outside the app's region has 225 to 245 KB in its largest block
 * beside a scene, and the recorder's own buffers come from there too. */
#define FRAMEREC_RING_BYTES    (96U * 1024U)
#define FRAMEREC_HEAD_MAX      40 /* "FRAMEREC_BIN 2048 ffffffff\n" with room */
#define FRAMEREC_PAYLOAD_MAX   (CONSOLE_EMIT_BYTES_MAX - FRAMEREC_HEAD_MAX)
/* Below the console task (4), above the frame loop (1). */
#define FRAMEREC_TASK_PRIORITY 2
#define FRAMEREC_TASK_STACK    3072
#define FRAMEREC_DRAIN_WAIT_MS 20
/* How long a held frame waits for ring room before it is dropped after all:
 * long enough for a reader to drain the ring, short enough that a reader
 * gone away does not hold the loop. */
#define FRAMEREC_HELD_WAIT_MS  5000
#define FRAMEREC_ARGS_MAX      (sizeof "start" - 1)

typedef struct {
    RingbufHandle_t ring;
    uint32_t taken, dropped;
    uint64_t sent;
    volatile bool stopping; /* no more frames; the task drains and ends */
    uint8_t* packet;
} sink_t;

static sink_t sink;
static TaskHandle_t drain_task;

static bool
sink_write(void* state, const uint8_t* bytes, size_t count) {
    sink_t* s = state;
    if (s->stopping) {
        return false;
    }
    const TickType_t wait = console_freeze_held() ? pdMS_TO_TICKS(FRAMEREC_HELD_WAIT_MS) : 0;
    if (xRingbufferSend(s->ring, bytes, count, wait) != pdTRUE) {
        s->dropped++;
        return false;
    }
    s->taken++;
    return true;
}

static void
send_packet(const uint8_t* bytes, size_t count) {
    const int head = snprintf((char*)sink.packet, FRAMEREC_HEAD_MAX, "FRAMEREC_BIN %u %08lx\n", (unsigned)count,
                              (unsigned long)crc32_update(0, bytes, count));
    memcpy(sink.packet + head, bytes, count);
    console_emit_bytes(sink.packet, (size_t)head + count);
    sink.sent += count;
}

static void
finish(void) {
    char line[96];
    (void)snprintf(line, sizeof line, "FRAMEREC_END frames=%lu dropped=%lu bytes=%llu", (unsigned long)sink.taken,
                   (unsigned long)sink.dropped, (unsigned long long)sink.sent);
    console_emit_line(line, "");
    vRingbufferDeleteWithCaps(sink.ring);
    memory_free(sink.packet);
    sink = (sink_t){0};
}

static void
drain(void* arg) {
    (void)arg;
    for (;;) {
        if (sink.ring == NULL) {
            vTaskDelay(pdMS_TO_TICKS(FRAMEREC_DRAIN_WAIT_MS));
            continue;
        }
        size_t count = 0;
        uint8_t* bytes =
            xRingbufferReceiveUpTo(sink.ring, &count, pdMS_TO_TICKS(FRAMEREC_DRAIN_WAIT_MS), FRAMEREC_PAYLOAD_MAX);
        if (bytes != NULL) {
            send_packet(bytes, count);
            vRingbufferReturnItem(sink.ring, bytes);
        } else if (sink.stopping) {
            finish();
        }
    }
}

static void
start(void) {
    if (sink.ring != NULL) {
        console_emit_line("FRAMEREC_REFUSED: already recording", "");
        return;
    }
    sink = (sink_t){0};
    sink.ring = xRingbufferCreateWithCaps(FRAMEREC_RING_BYTES, RINGBUF_TYPE_BYTEBUF, MALLOC_CAP_SPIRAM);
    sink.packet = memory_alloc(CONSOLE_EMIT_BYTES_MAX, MEMORY_PSRAM);
    if (drain_task == NULL) {
        (void)xTaskCreate(drain, "framerec", FRAMEREC_TASK_STACK, NULL, FRAMEREC_TASK_PRIORITY, &drain_task);
    }
    if (sink.ring == NULL || sink.packet == NULL || drain_task == NULL) {
        char why[96];
        (void)snprintf(why, sizeof why, "FRAMEREC_REFUSED: no memory: ring=%d packet=%d task=%d psram largest=%u",
                       sink.ring != NULL, sink.packet != NULL, drain_task != NULL,
                       (unsigned)memory_largest_block(MEMORY_PSRAM));
        console_emit_line(why, "");
        if (sink.ring != NULL) {
            vRingbufferDeleteWithCaps(sink.ring);
        }
        memory_free(sink.packet);
        sink = (sink_t){0};
        return;
    }
    char line[64];
    (void)snprintf(line, sizeof line, "FRAMEREC_BEGIN ring=%u keyframe_every=%d", (unsigned)FRAMEREC_RING_BYTES,
                   FRAME_RECORD_KEYFRAME_EVERY);
    console_emit_line(line, "");
    const frame_record_out_t out = {sink_write, &sink};
    if (!frame_record_start(&out, FRAME_RECORD_KEYFRAME_EVERY)) {
        console_emit_line("FRAMEREC_REFUSED: the recorder has no memory", "");
        sink.stopping = true;
    }
}

static void
answer(const void* payload) {
    const bool starting = *(const bool*)payload;
    if (starting) {
        start();
    } else if (sink.ring != NULL && !sink.stopping) {
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
