/*
 * console_stream: bytes written from one task go out from another as
 * console_emit_packet() packets, so the writer never waits on the link.
 * A task fills packets from a ring in external RAM or a pull source.
 * docs/tools/Console-Framing.md.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct console_stream console_stream_t;

/* Called from the stream's task once a closed stream has sent everything,
 * with its sent byte and packet counts. The stream is freed after it. */
typedef void (*console_stream_ended_fn)(uint64_t sent, uint64_t packets, void* arg);

/* A stream sending "<tag>_BIN" packets through a ring of `ring_bytes`, or
 * NULL when its ring, its packet buffer or its task does not fit. `tag`
 * must outlive the stream. */
console_stream_t* console_stream_open(const char* tag, size_t ring_bytes);

/* Fills one packet and observes the preceding write, if any. Zero leaves
 * the source idle, or ends it after close. */
typedef size_t (*console_stream_fill_fn)(void* arg, uint8_t* packet, size_t capacity, uint32_t write_us, bool sent);
/* The caller lends CONSOLE_PACKET_BYTES_MAX bytes for fill and
 * CONSOLE_PACKET_MAX for framing through the end callback. */
console_stream_t* console_stream_open_source(const char* tag, console_stream_fill_fn fill, void* arg, uint8_t* storage,
                                             uint8_t* packet, unsigned priority, int core);

/* Notify a pull source that data is ready without waiting on its sender. */
void console_stream_wake(console_stream_t* stream);

/* Queues `count` bytes whole, waiting up to `wait_ms` for ring room; false
 * leaves none of them queued. From one writer at a time. */
bool console_stream_write(console_stream_t* stream, const uint8_t* bytes, size_t count, unsigned wait_ms);

/* Stop producing bytes before closing. The task drains the stream, calls
 * `ended` (unless NULL) and frees the stream. */
void console_stream_close(console_stream_t* stream, console_stream_ended_fn ended, void* arg);
