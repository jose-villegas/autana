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
panelcap_init(panelcap_t* capture, uint8_t* storage, unsigned fps) {
    *capture = (panelcap_t){.storage = storage, .fps = fps};
    atomic_init(&capture->ready, 0);
    memset(storage, 0, PANELCAP_IMAGE_BYTES);
}

static void
drop(panelcap_t* capture) {
    capture->dropped++;
    if (capture->dropped_before < UINT16_MAX) {
        capture->dropped_before++;
    }
    capture->admitted = false;
}

bool
panelcap_begin(panelcap_t* capture, uint64_t us) {
    capture->seq++;
    capture->admitted = false;
    if (capture->fps == 0 || us < capture->next_us || atomic_load_explicit(&capture->ready, memory_order_acquire)) {
        drop(capture);
        return false;
    }
    capture->next_us = us + PANELCAP_US_PER_SECOND / capture->fps;
    capture->windows = 0;
    capture->collapsed = false;
    capture->admitted = true;
    bytes_write_u32(capture->head, capture->seq);
    bytes_write_u64(capture->head + sizeof(uint32_t), us);
    return true;
}

void
panelcap_window(panelcap_t* capture, unsigned x0, unsigned y0, unsigned x1, unsigned y1, const void* pixels) {
    if (!capture->admitted) {
        return;
    }
    if (pixels == NULL || x1 <= x0 || y1 <= y0 || x1 > PANELCAP_WIDTH || y1 > PANELCAP_HEIGHT) {
        drop(capture);
        return;
    }
    const uint8_t* source = pixels;
    size_t row_bytes = (x1 - x0) * PANELCAP_PIXEL_BYTES;
    for (unsigned y = y0; y < y1; y++) {
        memcpy(capture->storage + ((size_t)y * PANELCAP_WIDTH + x0) * PANELCAP_PIXEL_BYTES, source, row_bytes);
        source += row_bytes;
    }
    recti_t box = recti_from_edges(x0, y0, x1, y1);
    if (capture->collapsed) {
        capture->dirty[0] = recti_union(capture->dirty[0], box);
        return;
    }
    for (unsigned i = 0; i < capture->windows;) {
        recti_t old = capture->dirty[i];
        recti_t joined = recti_union(old, box);
        if (!recti_is_empty(recti_intersect(old, box)) || recti_area(joined) == recti_area(old) + recti_area(box)) {
            box = joined;
            capture->dirty[i] = capture->dirty[--capture->windows];
            i = 0;
        } else {
            i++;
        }
    }
    if (capture->windows == PANELCAP_WINDOWS_MAX) {
        /* A bounding box bounds metadata without encoding untouched panel margins. */
        for (unsigned i = 0; i < capture->windows; i++) {
            box = recti_union(box, capture->dirty[i]);
        }
        capture->windows = 0;
        capture->collapsed = true;
    }
    capture->dirty[capture->windows++] = box;
}

void
panelcap_commit(panelcap_t* capture) {
    if (!capture->admitted) {
        return;
    }
    bytes_write_u16(capture->head + sizeof(uint32_t) + sizeof(uint64_t), capture->windows);
    bytes_write_u16(capture->head + PANELCAP_HEAD_BYTES - sizeof(uint16_t), capture->dropped_before);
    capture->dropped_before = 0;
    capture->presents++;
    atomic_store_explicit(&capture->ready, 1, memory_order_release);
    capture->admitted = false;
}

const uint8_t*
panelcap_peek(panelcap_t* capture) {
    return atomic_load_explicit(&capture->ready, memory_order_acquire) ? capture->head : NULL;
}

void
panelcap_release(panelcap_t* capture) {
    atomic_store_explicit(&capture->ready, 0, memory_order_release);
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
panelcap_send(const panelcap_t* capture, uint8_t* packet, size_t capacity, panelcap_emit_fn emit, void* arg) {
    if (capacity <= PANELCAP_PART_HEAD_BYTES) {
        return;
    }
    parts_t parts = {
        .packet = packet, .used = PANELCAP_PART_HEAD_BYTES, .capacity = capacity, .emit = emit, .arg = arg};
    memcpy(packet, capture->head, sizeof(uint32_t));
    slice(&parts, capture->head, PANELCAP_HEAD_BYTES);
    for (unsigned i = 0; i < capture->windows; i++) {
        recti_t box = capture->dirty[i];
        uint8_t head[PANELCAP_WINDOW_BYTES];
        const unsigned edges[] = {box.x, box.y, recti_right(box), recti_bottom(box)};
        for (unsigned edge = 0; edge < PANELCAP_COORDS; edge++) {
            bytes_write_u16(head + edge * sizeof(uint16_t), edges[edge]);
        }
        size_t packed = 0;
        for (int y = box.y; y < recti_bottom(box); y++) {
            const uint8_t* row = capture->storage + ((size_t)y * PANELCAP_WIDTH + box.x) * PANELCAP_PIXEL_BYTES;
            packed += panelcap_pack(row, box.w, NULL, NULL);
        }
        bytes_write_u32(head + PANELCAP_COORDS * sizeof(uint16_t), packed);
        slice(&parts, head, sizeof head);
        for (int y = box.y; y < recti_bottom(box); y++) {
            const uint8_t* row = capture->storage + ((size_t)y * PANELCAP_WIDTH + box.x) * PANELCAP_PIXEL_BYTES;
            (void)panelcap_pack(row, box.w, slice, &parts);
        }
    }
    flush(&parts, true);
}
