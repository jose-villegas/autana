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
    const char* tag;
    console_stream_fill_fn fill;
    void* source_arg;
    console_stream_ended_fn ended; /* set with `closed`, before it */
    void* arg;
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

static void
stream_task(void* arg) {
    console_stream_t* stream = arg;
    uint64_t sent = 0;
    uint64_t packets = 0;
    for (;;) {
        const bool source = stream->fill != NULL;
        const bool closed = __atomic_load_n(&stream->closed, __ATOMIC_ACQUIRE);
        size_t count = 0;
        uint8_t* bytes;
        if (source) {
            bytes = stream->storage;
            count = stream->fill(stream->source_arg, bytes, CONSOLE_PACKET_BYTES_MAX);
        } else {
            bytes = xRingbufferReceiveUpTo(stream->ring, &count, pdMS_TO_TICKS(STREAM_IDLE_WAIT_MS),
                                           CONSOLE_PACKET_BYTES_MAX);
        }
        if (count != 0) {
            if (console_emit_packet(stream->tag, bytes, count, stream->packet, source)) {
                sent += count;
                packets++;
            } else if (source && closed) {
                break;
            }
            if (stream->ring != NULL) {
                vRingbufferReturnItem(stream->ring, bytes);
            }
        } else if (closed) {
            break;
        } else if (source) {
            timing_sleep_ms(STREAM_IDLE_WAIT_MS);
        }
    }
    if (stream->ended != NULL) {
        stream->ended(sent, packets, stream->arg);
    }
    stream_free(stream);
    vTaskDelete(NULL);
}

static console_stream_t*
open_stream(const char* tag, size_t bytes, console_stream_fill_fn fill, void* arg, uint8_t* storage, uint8_t* packet) {
    console_stream_t* stream = memory_alloc(sizeof *stream, MEMORY_INTERNAL);
    if (stream == NULL) {
        return NULL;
    }
    *stream = (console_stream_t){.tag = tag, .fill = fill, .source_arg = arg};
    stream->storage = fill == NULL ? memory_alloc(bytes, MEMORY_PSRAM) : storage;
    stream->packet = fill == NULL ? memory_alloc(CONSOLE_PACKET_MAX, MEMORY_PSRAM) : packet;
    if (fill == NULL && stream->storage != NULL) {
        stream->ring = xRingbufferCreateStatic(bytes, RINGBUF_TYPE_BYTEBUF, stream->storage, &stream->ring_state);
    }
    if (stream->storage == NULL || stream->packet == NULL || (fill == NULL && stream->ring == NULL)
        || xTaskCreate(stream_task, tag, STREAM_TASK_STACK, stream, STREAM_TASK_PRIORITY, NULL) != pdPASS) {
        stream_free(stream);
        return NULL;
    }
    return stream;
}

console_stream_t*
console_stream_open(const char* tag, size_t ring_bytes) {
    return open_stream(tag, ring_bytes, NULL, NULL, NULL, NULL);
}

console_stream_t*
console_stream_open_source(const char* tag, console_stream_fill_fn fill, void* arg, uint8_t* storage, uint8_t* packet) {
    return open_stream(tag, 0, fill, arg, storage, packet);
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
