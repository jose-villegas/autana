#include "gfx/present/capture/panelcap.h"
#include <limits.h>
#include <string.h>

size_t
panelcap_pack_bound(size_t pixels) {
    return pixels * PANELCAP_PIXEL_BYTES + (pixels + PANELCAP_PACK_RUN_MAX - 1) / PANELCAP_PACK_RUN_MAX;
}

static bool
same(const uint8_t* p, size_t a, size_t b) {
    return memcmp(p + a * PANELCAP_PIXEL_BYTES, p + b * PANELCAP_PIXEL_BYTES, PANELCAP_PIXEL_BYTES) == 0;
}

size_t
panelcap_pack(const uint8_t* pixels, size_t count, panelcap_emit_fn emit, void* arg) {
    size_t total = 0;
    for (size_t at = 0; at < count;) {
        size_t run = 1;
        while (at + run < count && run < PANELCAP_PACK_RUN_MAX && same(pixels, at, at + run)) {
            run++;
        }
        bool repeat = run > 1;
        if (!repeat) {
            while (at + run < count && run < PANELCAP_PACK_RUN_MAX) {
                /* A two-pixel run costs as much as literals at a boundary;
                 * keeping it literal preserves the literal-size bound. */
                if (at + run + 2 < count && same(pixels, at + run, at + run + 1)
                    && same(pixels, at + run, at + run + 2)) {
                    break;
                }
                run++;
            }
        }
        uint8_t head = repeat ? (uint8_t)(1 - (int)run) : (uint8_t)(run - 1);
        size_t bytes = (repeat ? 1 : run) * PANELCAP_PIXEL_BYTES;
        if (emit != NULL) {
            emit(arg, &head, 1);
            emit(arg, pixels + at * PANELCAP_PIXEL_BYTES, bytes);
        }
        total += 1 + bytes;
        at += run;
    }
    return total;
}

void
panelcap_ring_init(panelcap_ring_t* ring, uint8_t* storage, unsigned fps) {
    *ring = (panelcap_ring_t){.storage = storage, .fps = fps};
    for (unsigned i = 0; i < PANELCAP_SLOTS; i++) {
        atomic_init(&ring->ready[i], 0);
    }
}

static void
drop(panelcap_ring_t* ring) {
    ring->dropped++;
    if (ring->dropped_before < UINT16_MAX) {
        ring->dropped_before++;
    }
    ring->admitted = false;
    ring->used = 0;
}

bool
panelcap_begin(panelcap_ring_t* ring, uint64_t us) {
    ring->seq++;
    ring->admitted = false;
    if (us < ring->next_us || atomic_load_explicit(&ring->ready[ring->write_slot], memory_order_acquire)) {
        drop(ring);
        return false;
    }
    ring->next_us = us + PANELCAP_US_PER_SECOND / ring->fps;
    ring->used = PANELCAP_HEAD_BYTES;
    ring->windows = 0;
    ring->admitted = true;
    uint8_t* head = ring->storage + ring->write_slot * PANELCAP_SLOT_BYTES;
    bytes_write_u32(head, ring->seq);
    bytes_write_u64(head + sizeof(uint32_t), us);
    return true;
}

void
panelcap_window(panelcap_ring_t* ring, unsigned x0, unsigned y0, unsigned x1, unsigned y1, const void* pixels) {
    if (!ring->admitted) {
        return;
    }
    size_t bytes = (size_t)(x1 - x0) * (y1 - y0) * PANELCAP_PIXEL_BYTES;
    if (x1 <= x0 || y1 <= y0 || x1 > PANELCAP_WIDTH || y1 > PANELCAP_HEIGHT || ring->windows == PANELCAP_WINDOWS_MAX
        || ring->used > PANELCAP_SLOT_BYTES - PANELCAP_WINDOW_BYTES
        || bytes > PANELCAP_SLOT_BYTES - ring->used - PANELCAP_WINDOW_BYTES) {
        drop(ring);
        return;
    }
    uint8_t* out = ring->storage + ring->write_slot * PANELCAP_SLOT_BYTES + ring->used;
    const unsigned edges[] = {x0, y0, x1, y1};
    for (unsigned i = 0; i < sizeof edges / sizeof edges[0]; i++) {
        bytes_write_u16(out + i * sizeof(uint16_t), edges[i]);
    }
    bytes_write_u32(out + PANELCAP_COORDS * sizeof(uint16_t), bytes);
    memcpy(out + PANELCAP_WINDOW_BYTES, pixels, bytes);
    ring->used += PANELCAP_WINDOW_BYTES + bytes;
    ring->windows++;
}

void
panelcap_commit(panelcap_ring_t* ring) {
    if (!ring->admitted) {
        return;
    }
    uint8_t* head = ring->storage + ring->write_slot * PANELCAP_SLOT_BYTES;
    bytes_write_u16(head + sizeof(uint32_t) + sizeof(uint64_t), ring->windows);
    bytes_write_u16(head + PANELCAP_HEAD_BYTES - sizeof(uint16_t), ring->dropped_before);
    ring->dropped_before = 0;
    ring->presents++;
    atomic_store_explicit(&ring->ready[ring->write_slot], 1, memory_order_release);
    ring->write_slot = (ring->write_slot + 1) % PANELCAP_SLOTS;
    ring->admitted = false;
}

const uint8_t*
panelcap_peek(panelcap_ring_t* ring) {
    return atomic_load_explicit(&ring->ready[ring->read_slot], memory_order_acquire)
               ? ring->storage + ring->read_slot * PANELCAP_SLOT_BYTES
               : NULL;
}

void
panelcap_release(panelcap_ring_t* ring) {
    atomic_store_explicit(&ring->ready[ring->read_slot], 0, memory_order_release);
    ring->read_slot = (ring->read_slot + 1) % PANELCAP_SLOTS;
}

typedef struct {
    uint8_t* packet;
    size_t used;
    size_t capacity;
    uint16_t part;
    panelcap_emit_fn emit;
    void* arg;
} parts_t;

static void
flush(parts_t* parts, bool last) {
    bytes_write_u16(parts->packet + sizeof(uint32_t), parts->part++);
    parts->packet[PANELCAP_PART_HEAD_BYTES - 1] = last;
    parts->emit(parts->arg, parts->packet, parts->used);
    parts->used = PANELCAP_PART_HEAD_BYTES;
}

static void
slice(void* arg, const uint8_t* bytes, size_t count) {
    parts_t* parts = arg;
    while (count != 0) {
        if (parts->used == parts->capacity) {
            flush(parts, false);
        }
        size_t take = parts->capacity - parts->used;
        if (take > count) {
            take = count;
        }
        memcpy(parts->packet + parts->used, bytes, take);
        parts->used += take;
        bytes += take;
        count -= take;
    }
}

void
panelcap_send(const uint8_t* present, uint8_t* packet, size_t capacity, panelcap_emit_fn emit, void* arg) {
    if (capacity <= PANELCAP_PART_HEAD_BYTES) {
        return;
    }
    parts_t parts = {
        .packet = packet, .used = PANELCAP_PART_HEAD_BYTES, .capacity = capacity, .emit = emit, .arg = arg};
    memcpy(packet, present, sizeof(uint32_t));
    slice(&parts, present, PANELCAP_HEAD_BYTES);
    unsigned windows = (unsigned)bytes_read_u16(present + sizeof(uint32_t) + sizeof(uint64_t));
    const uint8_t* bytes = present + PANELCAP_HEAD_BYTES;
    for (unsigned i = 0; i < windows; i++) {
        uint8_t head[PANELCAP_WINDOW_BYTES];
        memcpy(head, bytes, sizeof head);
        size_t pixels = bytes_read_u32(head + PANELCAP_COORDS * sizeof(uint16_t)) / PANELCAP_PIXEL_BYTES;
        bytes += PANELCAP_WINDOW_BYTES;
        size_t packed = panelcap_pack(bytes, pixels, NULL, NULL);
        bytes_write_u32(head + PANELCAP_COORDS * sizeof(uint16_t), packed);
        slice(&parts, head, sizeof head);
        (void)panelcap_pack(bytes, pixels, slice, &parts);
        bytes += pixels * PANELCAP_PIXEL_BYTES;
    }
    flush(&parts, true);
}
