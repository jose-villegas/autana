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
#define PANELCAP_COORDS          4U
#define PANELCAP_WINDOW_BYTES    12U
#define PANELCAP_WINDOWS_MAX     32U
#define PANELCAP_FPS_DEFAULT     15U
#define PANELCAP_FPS_MAX         60U
#define PANELCAP_US_PER_SECOND   1000000U
#define PANELCAP_PACK_RUN_MAX    128U
#define PANELCAP_PART_HEAD_BYTES 7U
#define PANELCAP_IMAGE_BYTES     GFX_FRAMEBUFFER_BYTES

typedef void (*panelcap_emit_fn)(void* arg, const uint8_t* data, size_t size);
size_t panelcap_pack_bound(size_t pixels);
size_t panelcap_pack(const uint8_t* pixels, size_t count, panelcap_emit_fn emit, void* arg);

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
    uint16_t dropped_before;
    uint64_t next_us;
    unsigned fps;
    bool admitted;
} panelcap_t;

void panelcap_init(panelcap_t* capture, uint8_t* storage, unsigned fps);
bool panelcap_begin(panelcap_t* capture, uint64_t us);
void panelcap_window(panelcap_t* capture, unsigned x0, unsigned y0, unsigned x1, unsigned y1, const void* pixels);
void panelcap_commit(panelcap_t* capture);
const uint8_t* panelcap_peek(panelcap_t* capture);
void panelcap_release(panelcap_t* capture);

void panelcap_send(const panelcap_t* capture, uint8_t* packet, size_t capacity, panelcap_emit_fn emit, void* arg);
