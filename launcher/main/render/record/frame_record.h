/*
 * frame_record: each drawn frame's render input as records
 * (render/record/frame_record_format.h), handed to a sink: what the renderer
 * was asked to draw, not pixels and not scene state, so a host redraws the
 * frame with the same renderer. Development and host builds only; in release
 * every call here is an empty inline.
 *
 * A frame is written whole or not at all: the sink's write gets one frame's
 * bytes in one call, and a refusal drops that frame, counts it, and makes the
 * next frame a keyframe. Every keyframe_every-th frame is a keyframe.
 *
 * A pack mesh is named by its pack and entry. A mesh the app made is told
 * here by whoever made it, before the draw: a skinned copy by the skinning
 * (its bind mesh and palette), a built mesh by its build (its source). The
 * compose tells it the picture, which a keyframe's CHECK hashes.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "build/build_variant.h"
#include "math/linear/mat4f.h"
#include "render/context/render_context.h"
#include "render/dynamic/r3d_dynamic_mesh.h"
#include "render/r3d_lit_mesh.h"

typedef struct {
    bool (*write)(void* sink, const uint8_t* bytes, size_t count); /* false: sink full, the frame is dropped */
    void* sink;
} frame_record_out_t;

/* A keyframe every this many frames: the most a reader seeking a frame
 * decodes before it. */
#define FRAME_RECORD_KEYFRAME_EVERY  30

/* The frame-cost bracket a recorded frame's records are composed and
 * handed to the sink in. */
#define FRAME_RECORD_STAGE_NAME      "frame.record"

/* Meshes the app made that a frame may draw, noted at once; past these the
 * oldest note gives way. */
#define FRAME_RECORD_NOTED_SKINS_MAX 64
#define FRAME_RECORD_NOTED_BUILT_MAX 4

typedef struct {
    uint32_t frames;  /* written */
    uint32_t dropped; /* refused by the sink */
} frame_record_counts_t;

#if BUILD_VARIANT_DEVELOPMENT_OR_HOST
/* Writes the stream header and records from the next frame on; false when
 * recording already runs or there is no memory for the frame buffer. */
bool frame_record_start(const frame_record_out_t* out, uint32_t keyframe_every);
void frame_record_stop(void);
bool frame_record_active(void);
frame_record_counts_t frame_record_counts(void);

/* The frame just drawn or generated through `context`. Called by the scene's
 * draw after render_context_draw(), which has made its choices. */
void frame_record_frame(const render_context_t* context, const raster_t* raster, uint32_t dt_ms);

/* Takes the table of notes below in external RAM, kept for the run; until
 * then nothing is noted. A development boot takes it before any scene
 * loads. False when it does not fit; true when already taken. */
bool frame_record_take_notes(void);

/* `posed` is `bind` posed by `palette`, `joints` matrices kept by the caller. */
void frame_record_note_skin(const r3d_lit_mesh_t* posed, const r3d_lit_mesh_t* bind, const mat4f_t* palette,
                            int joints);
/* `built` was built from `source`, whose arrays stay until the next build. */
void frame_record_note_built(const r3d_lit_mesh_t* built, const r3d_dynamic_source_t* source, bool fitted);
/* A mesh is being made in `block`: notes of meshes that lived there are stale. */
void frame_record_forget(const void* block, size_t bytes);
/* The picture the last frame composed into, `width` by `height`: hashed into
 * a CHECK when that frame was a keyframe, which the next frame carries. */
void frame_record_note_shown(const uint16_t* pixels, int width, int height);
#if BUILD_VARIANT_SELFTEST_OR_HOST
/* Forgets every noted skin and built mesh: a test's notes outlive its
 * meshes. The table stays taken. */
void frame_record_forget_notes(void);
#endif
#else
static inline bool
frame_record_start(const frame_record_out_t* out, uint32_t keyframe_every) {
    (void)out;
    (void)keyframe_every;
    return false;
}

static inline void
frame_record_stop(void) {}

static inline bool
frame_record_active(void) {
    return false;
}

static inline frame_record_counts_t
frame_record_counts(void) {
    return (frame_record_counts_t){0};
}

static inline void
frame_record_frame(const render_context_t* context, const raster_t* raster, uint32_t dt_ms) {
    (void)context;
    (void)raster;
    (void)dt_ms;
}

static inline void
frame_record_note_skin(const r3d_lit_mesh_t* posed, const r3d_lit_mesh_t* bind, const mat4f_t* palette, int joints) {
    (void)posed;
    (void)bind;
    (void)palette;
    (void)joints;
}

static inline void
frame_record_note_built(const r3d_lit_mesh_t* built, const r3d_dynamic_source_t* source, bool fitted) {
    (void)built;
    (void)source;
    (void)fitted;
}

static inline bool
frame_record_take_notes(void) {
    return true; /* nothing to take: a release build notes nothing */
}

static inline void
frame_record_forget(const void* block, size_t bytes) {
    (void)block;
    (void)bytes;
}

static inline void
frame_record_note_shown(const uint16_t* pixels, int width, int height) {
    (void)pixels;
    (void)width;
    (void)height;
}
#endif
