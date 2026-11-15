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
#include "console/console_stream.h"
#include "console/console_verbs.h"
#include "core/memory.h"
#include "render/record/frame_record.h"
#include "shell/console/console_freeze.h"

#include <stdio.h>
#include <string.h>
#include <strings.h>

/* A few of the largest frames measured (the water tour's, about 20 KB): the
 * PSRAM outside the app's region has 225 to 245 KB in its largest block
 * beside a scene, and the recorder's own buffers come from there too. */
#define FRAMEREC_RING_BYTES   (96U * 1024U)
/* How long a held frame waits for ring room before it is dropped after all:
 * long enough for a reader to drain the ring, short enough that a reader
 * gone away does not hold the loop. */
#define FRAMEREC_HELD_WAIT_MS 5000U
#define FRAMEREC_ARGS_MAX     (sizeof "start" - 1)
#define FRAMEREC_TAG          "FRAMEREC"
#define FRAMEREC_LINE_MAX     96

/* Set from START until the stream has sent FRAMEREC_END and ended. */
static console_stream_t* volatile stream;
static bool stopping;                /* STOP taken, the stream draining */
static frame_record_counts_t counts; /* the recorder's, taken at STOP */

static bool
sink_write(void* state, const uint8_t* bytes, size_t count) {
    return console_stream_write(state, bytes, count, console_freeze_held() ? FRAMEREC_HELD_WAIT_MS : 0U);
}

/* From the stream's task, once it has sent everything. */
static void
ended(uint64_t sent, void* arg) {
    (void)arg;
    char line[FRAMEREC_LINE_MAX];
    (void)snprintf(line, sizeof line, "FRAMEREC_END frames=%lu dropped=%lu bytes=%llu", (unsigned long)counts.frames,
                   (unsigned long)counts.dropped, (unsigned long long)sent);
    console_emit_line(line, "");
    stream = NULL;
}

static void
start(void) {
    if (stream != NULL) {
        console_emit_line("FRAMEREC_REFUSED: already recording", "");
        return;
    }
    console_stream_t* opened = console_stream_open(FRAMEREC_TAG, FRAMEREC_RING_BYTES);
    if (opened == NULL) {
        char why[FRAMEREC_LINE_MAX];
        (void)snprintf(why, sizeof why, "FRAMEREC_REFUSED: no memory for the stream: psram largest=%u",
                       (unsigned)memory_largest_block(MEMORY_PSRAM));
        console_emit_line(why, "");
        return;
    }
    stopping = false;
    counts = (frame_record_counts_t){0};
    stream = opened;
    if (!frame_record_start(&(frame_record_out_t){sink_write, opened}, FRAME_RECORD_KEYFRAME_EVERY)) {
        console_emit_line("FRAMEREC_REFUSED: the recorder has no memory", "");
        stream = NULL;
        console_stream_close(opened, NULL, NULL);
        return;
    }
    char line[FRAMEREC_LINE_MAX];
    (void)snprintf(line, sizeof line, "FRAMEREC_BEGIN ring=%u keyframe_every=%d", (unsigned)FRAMEREC_RING_BYTES,
                   FRAME_RECORD_KEYFRAME_EVERY);
    console_emit_line(line, "");
}

static void
answer(const void* payload) {
    const bool starting = *(const bool*)payload;
    if (starting) {
        start();
    } else if (stream != NULL && !stopping) {
        counts = frame_record_counts();
        frame_record_stop();
        stopping = true;
        console_stream_close(stream, ended, NULL);
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
