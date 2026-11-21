/* panelcap: bounded whole-present storage and pixel PackBits for panel capture. */
#pragma once

#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "collections/bytes.h"
#include "collections/bytes_write.h"

#define PANELCAP_WIDTH           368U
#define PANELCAP_HEIGHT          448U
#define PANELCAP_PIXEL_BYTES     2U
#define PANELCAP_HEAD_BYTES      16U
#define PANELCAP_COORDS          4U
#define PANELCAP_WINDOW_BYTES    12U
#define PANELCAP_WINDOWS_MAX     512U
#define PANELCAP_SLOTS           2U
#define PANELCAP_FPS_DEFAULT     15U
#define PANELCAP_FPS_MAX         60U
#define PANELCAP_US_PER_SECOND   1000000U
#define PANELCAP_PACK_RUN_MAX    128U
#define PANELCAP_PART_HEAD_BYTES 7U
/* Two full-panel passes cover overlay cleanup plus redraw; window metadata
 * has its own bound. Two slots let the sender own one while the next fills. */
#define PANELCAP_SLOT_BYTES                                                                                            \
    (PANELCAP_HEAD_BYTES + 2U * PANELCAP_WIDTH * PANELCAP_HEIGHT * PANELCAP_PIXEL_BYTES                                \
     + PANELCAP_WINDOWS_MAX * PANELCAP_WINDOW_BYTES)
#define PANELCAP_RING_BYTES (PANELCAP_SLOTS * PANELCAP_SLOT_BYTES)

typedef void (*panelcap_emit_fn)(void* arg, const uint8_t* data, size_t size);
size_t panelcap_pack_bound(size_t pixels);
size_t panelcap_pack(const uint8_t* pixels, size_t count, panelcap_emit_fn emit, void* arg);

typedef struct {
    uint8_t* storage;
    atomic_uint ready[PANELCAP_SLOTS];
    unsigned write_slot;
    unsigned read_slot;
    size_t used;
    uint16_t windows;
    uint32_t seq;
    uint32_t presents;
    uint32_t dropped;
    uint16_t dropped_before;
    uint64_t next_us;
    unsigned fps;
    bool admitted;
} panelcap_ring_t;

void panelcap_ring_init(panelcap_ring_t* ring, uint8_t* storage, unsigned fps);
bool panelcap_begin(panelcap_ring_t* ring, uint64_t us);
void panelcap_window(panelcap_ring_t* ring, unsigned x0, unsigned y0, unsigned x1, unsigned y1, const void* pixels);
void panelcap_commit(panelcap_ring_t* ring);
const uint8_t* panelcap_peek(panelcap_ring_t* ring);
void panelcap_release(panelcap_ring_t* ring);

void panelcap_send(const uint8_t* present, uint8_t* packet, size_t capacity, panelcap_emit_fn emit, void* arg);
