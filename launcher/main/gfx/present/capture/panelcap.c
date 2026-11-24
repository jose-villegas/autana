#include "gfx/present/capture/panelcap.h"
#include <limits.h>
#include <string.h>
#include "math/geometry/rect_overlap.h"
#include "math/scalar/units.h"

static bool
same(const uint8_t* p, size_t a, size_t b) {
    return memcmp(p + (a * PANELCAP_PIXEL_BYTES), p + (b * PANELCAP_PIXEL_BYTES), PANELCAP_PIXEL_BYTES) == 0;
}

static size_t
pack_block(uint8_t* block, const uint8_t* pixels, size_t count, size_t* consumed) {
    size_t run = 1;
    while (run < count && run < PANELCAP_PACK_RUN_MAX && same(pixels, 0, run)) {
        run++;
    }
    bool repeat = run > 1;
    if (!repeat) {
        while (run < count && run < PANELCAP_PACK_RUN_MAX) {
            /* Keep pairs inside a literal so their headers cannot increase its size. */
            if (run + 2 < count && same(pixels, run, run + 1) && same(pixels, run, run + 2)) {
                break;
            }
            run++;
        }
    }
    block[0] = repeat ? (uint8_t)(1 - (int)run) : (uint8_t)(run - 1);
    size_t bytes = (repeat ? 1 : run) * PANELCAP_PIXEL_BYTES;
    memcpy(block + 1, pixels, bytes);
    *consumed = run;
    return 1 + bytes;
}

bool
panelcap_init(panelcap_t* capture, uint8_t* storage, unsigned fps) {
    if (fps == 0 || fps > PANELCAP_FPS_MAX) {
        return false;
    }
    *capture = (panelcap_t){.storage = storage, .fps = fps};
    atomic_init(&capture->ready, 0);
    memset(storage, 0, PANELCAP_IMAGE_BYTES);
    return true;
}

static void
drop(panelcap_t* capture) {
    capture->dropped++;
    if (capture->dropped_before < UINT16_MAX) {
        capture->dropped_before++;
    }
}

static bool
take_image(panelcap_t* capture) {
    if (atomic_load_explicit(&capture->ready, memory_order_acquire)) {
        return false;
    }
    if (capture->sent) {
        capture->sent = false;
        capture->windows = 0;
        capture->collapsed = false;
    }
    return true;
}

bool
panelcap_begin(panelcap_t* capture, uint64_t us) {
    capture->present_us = us;
    capture->in_present = true;
    capture->had_window = false;
    capture->skipped_window = false;
    capture->writable = take_image(capture);
    if (!capture->writable || capture->redraw_pending || recti_is_empty(capture->owed)) {
        return false;
    }
    capture->redraw_pending = true;
    return true;
}

recti_t
panelcap_redraw_requested(panelcap_t* capture) {
    const recti_t box = capture->owed;
    capture->owed = recti_empty();
    capture->redraw_pending = false;
    return box;
}

/* A strip just copied covers the owed box or one whole edge of it: the owed box shrinks to what is left. */
static void
shrink_owed(panelcap_t* capture, recti_t box) {
    if (recti_is_empty(capture->owed)) {
        return;
    }
    recti_t left[RECTI_SUBTRACT_MAX];
    const int pieces = recti_subtract(capture->owed, box, left);
    if (pieces <= 1) {
        capture->owed = pieces == 1 ? left[0] : recti_empty();
    }
}

static void
add_dirty(panelcap_t* capture, recti_t box) {
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
panelcap_window(panelcap_t* capture, unsigned x0, unsigned y0, unsigned x1, unsigned y1, const void* pixels) {
    if (!capture->in_present || pixels == NULL || x1 <= x0 || y1 <= y0 || x1 > PANELCAP_WIDTH || y1 > PANELCAP_HEIGHT) {
        return;
    }
    recti_t box = recti_from_edges((int)x0, (int)y0, (int)x1, (int)y1);
    capture->had_window = true;
    if (!capture->writable) {
        capture->writable = take_image(capture);
        if (!capture->writable) {
            capture->skipped_window = true;
            capture->owed = recti_union(capture->owed, box);
            return;
        }
    }
    const uint8_t* source = pixels;
    size_t row_bytes = (x1 - x0) * PANELCAP_PIXEL_BYTES;
    capture->copied_bytes += row_bytes * (y1 - y0);
    for (unsigned y = y0; y < y1; y++) {
        memcpy(capture->storage + ((((size_t)y * PANELCAP_WIDTH) + x0) * PANELCAP_PIXEL_BYTES), source, row_bytes);
        source += row_bytes;
    }
    shrink_owed(capture, box);
    if (capture->collapsed) {
        capture->dirty[0] = recti_union(capture->dirty[0], box);
        return;
    }
    add_dirty(capture, box);
}

void
panelcap_commit(panelcap_t* capture) {
    if (!capture->in_present) {
        return;
    }
    capture->in_present = false;
    if (capture->had_window) {
        capture->seq++;
    }
    if (capture->skipped_window) {
        capture->dropped_busy++;
        drop(capture);
        return;
    }
    if (!capture->writable || capture->windows == 0) {
        return;
    }
    if (!recti_is_empty(capture->owed)) {
        if (capture->had_window) {
            capture->dropped_redraw++;
            drop(capture);
        }
        return;
    }
    if (capture->present_us < capture->next_us) {
        if (capture->had_window) {
            capture->dropped_fps++;
            drop(capture);
        }
        return;
    }
    capture->next_us = capture->present_us + (MATH_US_PER_S / capture->fps);
    if (capture->presents != 0 && capture->seq == bytes_read_u32(capture->head + PANELCAP_HEAD_AT_SEQ)) {
        /* The receiver may already have rejected the failed publication's sequence. */
        capture->seq++;
    }
    bytes_write_u32(capture->head + PANELCAP_HEAD_AT_SEQ, capture->seq);
    bytes_write_u64(capture->head + PANELCAP_HEAD_AT_US, capture->present_us);
    bytes_write_u16(capture->head + PANELCAP_HEAD_AT_WINDOWS, capture->windows);
    bytes_write_u16(capture->head + PANELCAP_HEAD_AT_DROPPED, capture->dropped_before);
    capture->dropped_before = 0;
    capture->presents++;
    atomic_store_explicit(&capture->ready, 1, memory_order_release);
}

const uint8_t*
panelcap_peek(panelcap_t* capture) {
    return atomic_load_explicit(&capture->ready, memory_order_acquire) ? capture->head : NULL;
}

void
panelcap_release(panelcap_t* capture, bool sent) {
    capture->sent = sent;
    atomic_store_explicit(&capture->ready, 0, memory_order_release);
}

static void
encode_block(const panelcap_t* capture, panelcap_encoder_t* encoder) {
    recti_t box = capture->dirty[encoder->window];
    if (!encoder->window_started) {
        bytes_write_u16(encoder->block + PANELCAP_WINDOW_AT_X0, box.x);
        bytes_write_u16(encoder->block + PANELCAP_WINDOW_AT_Y0, box.y);
        bytes_write_u16(encoder->block + PANELCAP_WINDOW_AT_X1, recti_right(box));
        bytes_write_u16(encoder->block + PANELCAP_WINDOW_AT_Y1, recti_bottom(box));
        encoder->used = PANELCAP_WINDOW_BYTES;
        encoder->window_started = true;
        encoder->x = box.x;
        encoder->y = box.y;
        return;
    }
    const uint8_t* pixels =
        capture->storage + ((((size_t)encoder->y * PANELCAP_WIDTH) + encoder->x) * PANELCAP_PIXEL_BYTES);
    size_t consumed;
    encoder->used = pack_block(encoder->block, pixels, recti_right(box) - encoder->x, &consumed);
    encoder->x += (int)consumed;
    if (encoder->x == recti_right(box)) {
        encoder->x = box.x;
        if (++encoder->y == recti_bottom(box)) {
            encoder->window++;
            encoder->window_started = false;
        }
    }
}

size_t
panelcap_fill(const panelcap_t* capture, panelcap_encoder_t* encoder, uint8_t* packet, size_t capacity) {
    if (capacity <= PANELCAP_PART_HEAD_BYTES) {
        return 0;
    }
    if (encoder->part == 0) {
        memcpy(encoder->block, capture->head, PANELCAP_HEAD_BYTES);
        encoder->used = PANELCAP_HEAD_BYTES;
    }
    size_t at = PANELCAP_PART_HEAD_BYTES;
    while (at < capacity) {
        if (encoder->offset == encoder->used) {
            if (encoder->window == capture->windows) {
                break;
            }
            encode_block(capture, encoder);
            encoder->offset = 0;
        }
        size_t take = capacity - at;
        if (take > encoder->used - encoder->offset) {
            take = encoder->used - encoder->offset;
        }
        memcpy(packet + at, encoder->block + encoder->offset, take);
        at += take;
        encoder->offset += take;
    }
    if (at == PANELCAP_PART_HEAD_BYTES) {
        return 0;
    }
    bytes_write_u32(packet + PANELCAP_PART_AT_SEQ, bytes_read_u32(capture->head + PANELCAP_HEAD_AT_SEQ));
    bytes_write_u16(packet + PANELCAP_PART_AT_INDEX, encoder->part++);
    packet[PANELCAP_PART_AT_LAST] = encoder->window == capture->windows && encoder->offset == encoder->used;
    return at;
}
