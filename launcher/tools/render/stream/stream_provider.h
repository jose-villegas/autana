/* stream_provider: a frame_provider_t that draws a frame record stream
 * (render/record/frame_record_format.h) again with the same renderer: from a
 * file (--stream PATH), or from bytes handed in as they arrive
 * (stream_feed()). Each frame's meshes are opened from the packs the stream
 * names, which must hold the same bytes (their crc32), posed, built and
 * drawn through the frame's own view, settings and size. A frame is drawn
 * whole, so any frame is drawn by decoding forward from the keyframe before
 * it, with no draws between.
 *
 * Options: --stream PATH, and --check, which fails the run when a drawn
 * frame's picture differs from the CHECK the stream carries for it. Packs
 * are read from the folder ASSET_STORE_DIR_ENV names (asset/asset_store.h). */
#ifndef STREAM_PROVIDER_H
#define STREAM_PROVIDER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "render_host.h"

extern const frame_provider_t stream_provider;

/* The stream's bytes as they arrive, in any pieces; a frame counts once it
 * is whole. False once the stream is refused: stream_status() says why. */
bool stream_feed(const uint8_t* bytes, size_t count);
/* Forgets the stream fed so far, for the next one. */
void stream_forget(void);

#define STREAM_BUILD_ID_BYTES 33
#define STREAM_WHY_BYTES      200

typedef struct {
    int frames;  /* whole frames received */
    int dropped; /* frames that followed a drop on the board */
    int checks_same, checks_differ;
    char build_id[STREAM_BUILD_ID_BYTES];
    bool refused;
    char why[STREAM_WHY_BYTES];
} stream_status_t;

stream_status_t stream_status(void);

/* The next draw draws frame `index`; false past the last whole frame. */
bool stream_seek(int index);
/* The frame the next draw draws. */
int stream_next(void);

typedef enum {
    STREAM_CHECK_NONE,       /* no CHECK names it, or it was not drawn since */
    STREAM_CHECK_SAME,       /* its picture's crc32 is the CHECK's */
    STREAM_CHECK_DIFFERS,    /* it is not */
    STREAM_CHECK_OTHER_SIZE, /* composed into a picture of another size than the recorded one */
} stream_check_t;

typedef struct {
    uint32_t dt_ms;
    uint8_t flags; /* FRAME_RECORD_KEYFRAME, _AFTER_DROP, _GENERATED */
    stream_check_t check;
} stream_frame_info_t;

/* What frame `index` holds; zeroed past the last whole frame. */
stream_frame_info_t stream_frame_info(int index);

#endif
