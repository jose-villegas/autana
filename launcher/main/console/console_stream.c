/* console_stream: see console_stream.h. */
#include "console/console_stream.h"

#include "console/console.h"
#include "core/memory.h"
#include "core/timing.h"

#include "freertos/FreeRTOS.h"
#include "freertos/ringbuf.h"
#include "freertos/task.h"

/* Below the console task (4), above the frame loop (1). */
#define STREAM_TASK_PRIORITY 2
#define STREAM_TASK_STACK    3072
/* How often an idle task looks whether the stream was closed. */
#define STREAM_IDLE_WAIT_MS  20

/* The task owns a stream from open until it frees it. The ring's own state
 * stays on chip, while a pull source lends its external buffers. */
struct console_stream {
    StaticRingbuffer_t ring_state;
    RingbufHandle_t ring;
    uint8_t* storage;
    uint8_t* packet;
    TaskHandle_t task;
    const char* tag;
    console_stream_fill_fn fill;
    void* source_arg;
    console_stream_ended_fn ended; /* set with `closed`, before it */
    void* arg;
    bool bounded;
    bool closed;
};

static void
stream_free(console_stream_t* stream) {
    if (stream->ring != NULL) {
        vRingbufferDelete(stream->ring);
    }
    if (stream->fill == NULL) {
        memory_free(stream->storage);
        memory_free(stream->packet);
    }
    memory_free(stream);
}

static bool
stream_send(console_stream_t* stream, uint8_t* bytes, size_t count) {
    const bool sent = console_emit_packet(stream->tag, bytes, count, stream->packet, stream->bounded);
    if (stream->ring != NULL) {
        vRingbufferReturnItem(stream->ring, bytes);
    }
    return sent;
}

/* One packet's bytes: a pull source fills its own buffer, a ring stream
 * takes what its writers queued. Returns the byte count, 0 when idle. */
static size_t
stream_next(console_stream_t* stream, uint8_t** bytes, uint32_t write_us, bool previous_sent) {
    size_t count = 0;
    if (stream->fill != NULL) {
        *bytes = stream->storage;
        return stream->fill(stream->source_arg, *bytes, CONSOLE_PACKET_BYTES_MAX, write_us, previous_sent);
    }
    *bytes = xRingbufferReceiveUpTo(stream->ring, &count, pdMS_TO_TICKS(STREAM_IDLE_WAIT_MS), CONSOLE_PACKET_BYTES_MAX);
    return count;
}

static void
stream_task(void* arg) {
    console_stream_t* stream = arg;
    const bool source = stream->fill != NULL;
    uint64_t sent = 0;
    uint64_t packets = 0;
    uint32_t write_us = 0;
    bool previous_sent = false;
    for (;;) {
        const bool closed = __atomic_load_n(&stream->closed, __ATOMIC_ACQUIRE);
        uint8_t* bytes = NULL;
        const size_t count = stream_next(stream, &bytes, write_us, previous_sent);
        if (count == 0) {
            if (closed) {
                break;
            }
            if (source) {
                (void)ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(STREAM_IDLE_WAIT_MS));
            }
            continue;
        }
        const int64_t started = timing_now_us();
        previous_sent = stream_send(stream, bytes, count);
        write_us = (uint32_t)(timing_now_us() - started);
        sent += previous_sent ? count : 0;
        packets += previous_sent ? 1 : 0;
    }
    if (stream->ended != NULL) {
        stream->ended(sent, packets, stream->arg);
    }
    stream_free(stream);
    vTaskDelete(NULL);
}

static console_stream_t*
open_stream(const char* tag, size_t bytes, console_stream_fill_fn fill, void* arg, uint8_t* storage, uint8_t* packet,
            bool bounded, unsigned priority, int core) {
    console_stream_t* stream = memory_alloc(sizeof *stream, MEMORY_INTERNAL);
    if (stream == NULL) {
        return NULL;
    }
    *stream = (console_stream_t){.tag = tag, .fill = fill, .source_arg = arg, .bounded = bounded};
    stream->storage = fill == NULL ? memory_alloc(bytes, MEMORY_PSRAM) : storage;
    stream->packet = fill == NULL ? memory_alloc(CONSOLE_PACKET_MAX, MEMORY_PSRAM) : packet;
    if (fill == NULL && stream->storage != NULL) {
        stream->ring = xRingbufferCreateStatic(bytes, RINGBUF_TYPE_BYTEBUF, stream->storage, &stream->ring_state);
    }
    if (stream->storage == NULL || stream->packet == NULL || (fill == NULL && stream->ring == NULL)
        || xTaskCreatePinnedToCore(stream_task, tag, STREAM_TASK_STACK, stream, priority, &stream->task, core)
               != pdPASS) {
        stream_free(stream);
        return NULL;
    }
    return stream;
}

console_stream_t*
console_stream_open(const char* tag, size_t ring_bytes) {
    return open_stream(tag, ring_bytes, NULL, NULL, NULL, NULL, false, STREAM_TASK_PRIORITY, tskNO_AFFINITY);
}

console_stream_t*
console_stream_open_source(const char* tag, console_stream_fill_fn fill, void* arg, uint8_t* storage, uint8_t* packet,
                           unsigned priority, int core) {
    return open_stream(tag, 0, fill, arg, storage, packet, true, priority, core);
}

void
console_stream_wake(console_stream_t* stream) {
    xTaskNotifyGive(stream->task);
}

bool
console_stream_write(console_stream_t* stream, const uint8_t* bytes, size_t count, unsigned wait_ms) {
    return xRingbufferSend(stream->ring, bytes, count, pdMS_TO_TICKS(wait_ms)) == pdTRUE;
}

void
console_stream_close(console_stream_t* stream, console_stream_ended_fn ended, void* arg) {
    stream->ended = ended;
    stream->arg = arg;
    __atomic_store_n(&stream->closed, true, __ATOMIC_RELEASE);
}
