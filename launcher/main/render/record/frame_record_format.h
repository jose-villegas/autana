/*
 * frame_record_format: the bytes of a frame record stream, written by
 * frame_record.c and read by launcher/tools/device/framerec.py. Every record
 * is a frame_record_head_t, then `length` bytes, little-endian, so a reader
 * skips a type it does not know. A stream opens with HEADER; each frame opens
 * with FRAME and holds its records until the next FRAME. A keyframe holds
 * everything a redraw needs; the frames after it hold what changed.
 *
 * Packs are named once each, by PACK, and a mesh then names its pack by slot
 * and its entry by index: the reader loads the same pack bytes, which the
 * PACK's crc32 proves. Floats are the engine's own, matrices whole.
 */
#pragma once

#include <stdint.h>

#define FRAME_RECORD_VERSION        1
#define FRAME_RECORD_BUILD_ID_BYTES 32
#define FRAME_RECORD_PACK_NAME      32
#define FRAME_RECORD_MATRIX         16 /* floats of a mat4f_t */
#define FRAME_RECORD_TUNE_NAME      32 /* core/tune.h's TUNE_NAME_MAX, NUL-padded, unterminated at 32 */
/* A pack slot or entry with nothing behind it: memory the app made. */
#define FRAME_RECORD_NO_PACK        0xFFU
#define FRAME_RECORD_NO_ENTRY       0xFFFFFFFFU

#define FRAME_RECORD_KEYFRAME       1U /* FRAME flag: everything a redraw needs follows */
#define FRAME_RECORD_AFTER_DROP     2U /* FRAME flag: frames before this one were dropped */
#define FRAME_RECORD_GENERATED      4U /* FRAME flag: warped from the last drawn frame, no MESH */

typedef enum {
    FRAME_RECORD_HEADER = 1,
    FRAME_RECORD_PACK = 2,
    FRAME_RECORD_FRAME = 3,
    FRAME_RECORD_VIEW = 4, /* the frame's render_view_t, as the engine holds it */
    FRAME_RECORD_CHOICE = 5,
    FRAME_RECORD_MESH = 6,
    FRAME_RECORD_POSE = 7,    /* frame_record_pose_t, then float[16] per joint */
    FRAME_RECORD_DYNMESH = 8, /* frame_record_dynmesh_t, then float[3] and u8[3] per vertex, u16[3] per triangle */
    FRAME_RECORD_TUNE = 9,    /* a tunable's value: on a keyframe each off its initial, else each that changed */
    FRAME_RECORD_CHECK = 10,  /* the crc32 of a keyframe's composed picture, in a later frame's records */
} frame_record_type_t;

/* Where a drawn mesh came from. */
typedef enum {
    FRAME_RECORD_STATIC = 0,  /* a pack mesh as it is */
    FRAME_RECORD_BATCH = 1,   /* a pack mesh's copies, placed by a pack batch */
    FRAME_RECORD_SKINNED = 2, /* a pack mesh posed by the POSE of the same index */
    FRAME_RECORD_BUILT = 3,   /* built at run time from the DYNMESH of the same index */
} frame_record_kind_t;

typedef struct __attribute__((packed)) {
    uint16_t type;
    uint32_t length;
} frame_record_head_t;

typedef struct __attribute__((packed)) {
    uint16_t version, panel_width, panel_height, keyframe_every;
    char build_id[FRAME_RECORD_BUILD_ID_BYTES];
} frame_record_header_t;

typedef struct __attribute__((packed)) {
    uint8_t slot;
    uint32_t crc32;
    char name[FRAME_RECORD_PACK_NAME];
} frame_record_pack_t;

typedef struct __attribute__((packed)) {
    uint32_t index, dt_ms;
    uint8_t flags;
    uint16_t instances, batches;
} frame_record_frame_t;

typedef struct __attribute__((packed)) {
    int16_t step;
    uint8_t generated, scale_percent;
    uint16_t clear;
} frame_record_choice_t;

/* A pack entry: FRAME_RECORD_NO_PACK and FRAME_RECORD_NO_ENTRY for none. */
typedef struct __attribute__((packed)) {
    uint8_t pack;
    uint32_t entry;
} frame_record_where_t;

/* Instances first, then batches, each numbered from 0 in that order. */
typedef struct __attribute__((packed)) {
    uint16_t index;
    uint8_t kind, placed;
    frame_record_where_t positions, colours, batch;
    float model[FRAME_RECORD_MATRIX];
} frame_record_mesh_t;

typedef struct __attribute__((packed)) {
    uint16_t index, joints;
} frame_record_pose_t;

typedef struct __attribute__((packed)) {
    uint16_t index;
    uint8_t fitted, double_sided;
    int32_t position_scale;
    uint32_t vertices, triangles;
} frame_record_dynmesh_t;

typedef struct __attribute__((packed)) {
    char name[FRAME_RECORD_TUNE_NAME];
    int32_t value;
} frame_record_tune_t;

/* The picture composed after frame `index` was drawn: the half picture or
 * the framebuffer, whichever the compose wrote, rows top to bottom. */
typedef struct __attribute__((packed)) {
    uint32_t index;
    uint16_t width, height;
    uint32_t crc32;
} frame_record_check_t;
