/* panelcap: bounded whole-present storage and pixel PackBits for panel capture. */
#pragma once

#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "collections/bytes.h"
#include "collections/bytes_write.h"
#include "gfx/gfx.h"

#define PANELCAP_WIDTH           GFX_WIDTH
#define PANELCAP_HEIGHT          GFX_HEIGHT
#define PANELCAP_PIXEL_BYTES     2U
#define PANELCAP_HEAD_BYTES      16U
#define PANELCAP_HEAD_AT_SEQ     0U
#define PANELCAP_HEAD_AT_US      4U
#define PANELCAP_HEAD_AT_WINDOWS 12U
#define PANELCAP_HEAD_AT_DROPPED 14U
#define PANELCAP_WINDOW_BYTES    8U
#define PANELCAP_WINDOW_AT_X0    0U
#define PANELCAP_WINDOW_AT_Y0    2U
#define PANELCAP_WINDOW_AT_X1    4U
#define PANELCAP_WINDOW_AT_Y1    6U
#define PANELCAP_WINDOWS_MAX     32U
#define PANELCAP_FPS_DEFAULT     15U
#define PANELCAP_FPS_MAX         60U
#define PANELCAP_PACK_RUN_MAX    128U
#define PANELCAP_PART_HEAD_BYTES 7U
#define PANELCAP_PART_AT_SEQ     0U
#define PANELCAP_PART_AT_INDEX   4U
#define PANELCAP_PART_AT_LAST    6U
#define PANELCAP_IMAGE_BYTES     GFX_FRAMEBUFFER_BYTES

typedef struct {
    uint8_t* storage;
    atomic_uint ready;
    uint8_t head[PANELCAP_HEAD_BYTES];
    recti_t dirty[PANELCAP_WINDOWS_MAX];
    bool collapsed;
    uint16_t windows;
    uint32_t seq;
    uint32_t presents;
    uint32_t dropped;
    uint32_t dropped_fps;
    uint32_t dropped_busy;
    uint32_t dropped_redraw;
    uint64_t copied_bytes;
    uint16_t dropped_before;
    uint64_t next_us;
    unsigned fps;
    uint64_t present_us;
    bool in_present;
    bool had_window;
    bool skipped_window;
    bool writable;
    recti_t owed;
    bool redraw_pending;
    bool sent;
} panelcap_t;

bool panelcap_init(panelcap_t* capture, uint8_t* storage, unsigned fps);
/* Returns true when the frame owner must request a redraw. */
bool panelcap_begin(panelcap_t* capture, uint64_t us);
/* Takes the lost windows' bounds after the frame owner drains the in-flight present. */
recti_t panelcap_redraw_requested(panelcap_t* capture);
void panelcap_window(panelcap_t* capture, unsigned x0, unsigned y0, unsigned x1, unsigned y1, const void* pixels);
void panelcap_commit(panelcap_t* capture);
const uint8_t* panelcap_peek(panelcap_t* capture);
/* A failed send keeps its dirty windows for the next publication. */
void panelcap_release(panelcap_t* capture, bool sent);

typedef struct {
    uint16_t part;
    unsigned window;
    bool window_started;
    int x;
    int y;
    uint8_t block[1 + PANELCAP_PACK_RUN_MAX * PANELCAP_PIXEL_BYTES];
    size_t used;
    size_t offset;
} panelcap_encoder_t;

/* Starts with a zeroed cursor. After the last packet, fill returns zero. */
size_t panelcap_fill(const panelcap_t* capture, panelcap_encoder_t* encoder, uint8_t* packet, size_t capacity);
