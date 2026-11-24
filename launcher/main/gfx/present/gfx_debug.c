#include "gfx/present/gfx_debug.h"
#include "core/memory.h"
#include "core/timing.h"
#include "gfx/present/gfx_internal.h"
#include "gfx/present/gfx_present_guard.h"
#include "gfx/target/gfx_color.h"

#include <stdlib.h>
#include <string.h>

#if CONFIG_LAUNCHER_DEVELOPMENT
#include "esp_log.h"
#include "image/pixel/rgb565.h"

static const char* TAG = "gfx";

/* See gfx_debug.h for why this is development-only. Used by gfx_set_debug_overlay()
 * below. */
bool debug_overlay_on;

bool
gfx_debug_overlay(void) {
    return debug_overlay_on;
}

bool leaf_overlay_on;

bool
gfx_debug_leaf_overlay(void) {
    return leaf_overlay_on;
}

/* See send_partial_band() for third path. Exists for device test. Not reset
 * by gfx_present(). */
int dev_strips_sent_full;
int dev_strips_sent_gathered;
int dev_strips_sent_partial;

void
gfx_reset_strip_send_counts(void) {
    dev_strips_sent_full = 0;
    dev_strips_sent_gathered = 0;
    dev_strips_sent_partial = 0;
    dev_bytes_sent = 0;
}

void
gfx_get_strip_send_counts(int* full_bands, int* gathered, int* partial_bands) {
    if (full_bands) {
        *full_bands = dev_strips_sent_full;
    }
    if (gathered) {
        *gathered = dev_strips_sent_gathered;
    }
    if (partial_bands) {
        *partial_bands = dev_strips_sent_partial;
    }
}

void
mark_rect_border(gfx_color_t* buf, int stride, int w, int h, gfx_color_t colour) {
    for (int col = 0; col < w; col++) {
        buf[col] = colour;
        buf[((size_t)(h - 1) * stride) + col] = colour;
    }
    for (int row = 0; row < h; row++) {
        buf[(size_t)row * stride] = colour;
        buf[((size_t)row * stride) + (w - 1)] = colour;
    }
}

/* Used for full-width send border. No scratch copy: direct to present_fb. Inverse
 * save/restore. */
#define BORDER_PIXELS      (2 * (COL_WIDTH + STRIP_HEIGHT))

#define LEAF_BORDER_PIXELS (2 * (LEAF_W + LEAF_H))

static void
save_border(const gfx_color_t* buf, int stride, int w, int h, gfx_color_t* out) {
    int i = 0;
    for (int col = 0; col < w; col++) {
        out[i++] = buf[col];
        out[i++] = buf[((size_t)(h - 1) * stride) + col];
    }
    for (int row = 0; row < h; row++) {
        out[i++] = buf[(size_t)row * stride];
        out[i++] = buf[((size_t)row * stride) + (w - 1)];
    }
}

static void
restore_border(gfx_color_t* buf, int stride, int w, int h, const gfx_color_t* saved) {
    int i = 0;
    for (int col = 0; col < w; col++) {
        buf[col] = saved[i++];
        buf[((size_t)(h - 1) * stride) + col] = saved[i++];
    }
    for (int row = 0; row < h; row++) {
        buf[(size_t)row * stride] = saved[i++];
        buf[((size_t)row * stride) + (w - 1)] = saved[i++];
    }
}

/* The save scratch and the overlay view below, borrowed from gather_buf's
 * front rather than malloc'd separately: a separate allocation competes for the one
 * contiguous 41 KB block an app needs, and a debug overlay must
 * never be why an app cannot start. gather_buf holds nothing else:
 * gathered runs pack into strip_bounce[], and the frame loop is
 * single-threaded. The _Static_assert ties the fit to
 * GATHER_MAX_PIXELS, so breaking it is a compile error rather than a
 * silent DMA overflow. */
#define OVERLAY_CELL_SAVE_PIXELS (GRID_COLS * BORDER_PIXELS)
#define OVERLAY_LEAF_SAVE_PIXELS (LEAF_RECTS_PER_ROW_MAX * LEAF_BORDER_PIXELS)

/* What this present's overlay outlines: each cell's dirty box (empty when
 * clean) and the leaf bits, copied before overlay_take_off_last_borders()
 * marks last present's bordered boxes dirty. The borders show only what
 * changed, and the box recorded for the next take-off never grows by the
 * take-off's own marks. */
typedef struct {
    recti_t cell[CELL_COUNT];
    uint16_t leaves[STRIP_COUNT * LEAF_SUB];
} overlay_view_t;

#define OVERLAY_VIEW_PIXELS ((sizeof(overlay_view_t) + sizeof(gfx_color_t) - 1) / sizeof(gfx_color_t))
_Static_assert(OVERLAY_CELL_SAVE_PIXELS + OVERLAY_LEAF_SAVE_PIXELS + OVERLAY_VIEW_PIXELS <= GATHER_MAX_PIXELS,
               "overlay scratch must fit inside gather_buf");

static inline gfx_color_t (*overlay_cell_save(void))
    [BORDER_PIXELS] { return (gfx_color_t(*)[BORDER_PIXELS])gather_buf; }

static inline gfx_color_t (*overlay_leaf_save(void))
    [LEAF_BORDER_PIXELS] { return (gfx_color_t(*)[LEAF_BORDER_PIXELS])(gather_buf + OVERLAY_CELL_SAVE_PIXELS); }

static inline overlay_view_t* overlay_view(void) {
    return (overlay_view_t*)(gather_buf + OVERLAY_CELL_SAVE_PIXELS + OVERLAY_LEAF_SAVE_PIXELS);
}

/* Shared by send_full_row() and gather_and_send(), never live at once:
 * the frame loop is single-threaded. Read while the save scratch in
 * gather_buf is live, so it cannot borrow that buffer. Malloc'd on
 * enable/disable (1024 bytes)
 * rather than static: too big for app_main()'s stack, and a permanent
 * .bss reservation fares no better given this repo's history of
 * static-growth OOMs. */
recti_t* leaf_rect_scratch;

void
gfx_set_debug_overlay(bool on) {
    GFX_PRESENT_GUARD();
    debug_overlay_on = on;
}

void
gfx_set_leaf_overlay(bool on) {
    GFX_PRESENT_GUARD();
    if (on) {
        if (leaf_rect_scratch == NULL) {
            leaf_rect_scratch = malloc(sizeof(*leaf_rect_scratch) * LEAF_RECTS_PER_ROW_MAX);
            if (leaf_rect_scratch == NULL) {
                ESP_LOGE(TAG,
                         "overlay: could not allocate %u-byte leaf "
                         "rect scratch - staying off",
                         (unsigned)(sizeof(*leaf_rect_scratch) * LEAF_RECTS_PER_ROW_MAX));
                return;
            }
        }
        leaf_overlay_on = true;
    } else {
        leaf_overlay_on = false;
        free(leaf_rect_scratch);
        leaf_rect_scratch = NULL;
    }
}

#ifdef ESP_PLATFORM
/* `send_shadow` holds every pixel handed to the panel, so after a present
 * it must equal `present_fb`. A difference is a pixel never sent, or one copied out
 * of PSRAM wrong (also counted on its own). A glitch this stays silent on
 * lies past the send buffers, on the QSPI link. */
static gfx_color_t* send_shadow;
static bool send_audit_primed;
static int64_t send_audit_uncovered_px, send_audit_uncovered_total;
static int64_t send_audit_copy_fault_px;
static int send_audit_first_x = -1, send_audit_first_y = -1;
static int64_t send_audit_log_at_us;

#define SEND_AUDIT_LOG_US 1000000

bool
gfx_send_audit(void) {
    return send_shadow != NULL;
}

int64_t
gfx_send_audit_uncovered_px(void) {
    return send_audit_uncovered_total;
}

void
gfx_set_send_audit(bool on) {
    GFX_PRESENT_GUARD();
    gfx_present_drain();
    if (on == (send_shadow != NULL)) {
        return;
    }
    if (!on) {
        memory_free(send_shadow);
        send_shadow = NULL;
        return;
    }
    send_shadow = memory_alloc(GFX_FRAMEBUFFER_BYTES, MEMORY_PSRAM);
    if (send_shadow == NULL) {
        ESP_LOGE(TAG, "send audit: no room for the shadow framebuffer - staying off");
        return;
    }
    send_audit_primed = false;
    send_audit_uncovered_total = 0;
}

static int
count_differing_px(const gfx_color_t* a, const gfx_color_t* b, int n) {
    int count = 0;
    for (int i = 0; i < n; i++) {
        count += a[i] != b[i];
    }
    return count;
}

static gfx_send_window_t* send_window_log;
static int send_window_capacity, send_window_count;

void
gfx_set_send_window_log(gfx_send_window_t* log, int capacity) {
    GFX_PRESENT_GUARD();
    gfx_present_drain();
    send_window_log = log;
    send_window_capacity = log == NULL ? 0 : capacity;
    send_window_count = 0;
}

int
gfx_send_window_count(void) {
    return send_window_count;
}

/* `buf` is what is about to go to the panel for [x0, x0+w) x [y0, y0+h),
 * already copied out of `present_fb`. */
void
send_audit_capture(int x0, int y0, int w, int h, const gfx_color_t* buf) {
    if (send_window_count < send_window_capacity) {
        send_window_log[send_window_count++] =
            (gfx_send_window_t){(int16_t)x0, (int16_t)y0, (int16_t)(x0 + w), (int16_t)(y0 + h)};
    }
    if (send_shadow == NULL) {
        return;
    }
    for (int r = 0; r < h; r++) {
        const gfx_color_t* src = buf + ((size_t)r * w);
        const gfx_color_t* fb_row = present_fb + ((size_t)(y0 + r) * GFX_WIDTH) + x0;
        if (memcmp(src, fb_row, (size_t)w * sizeof(gfx_color_t)) != 0) {
            send_audit_copy_fault_px += count_differing_px(src, fb_row, w);
        }
        memcpy(send_shadow + ((size_t)(y0 + r) * GFX_WIDTH) + x0, src, (size_t)w * sizeof(gfx_color_t));
    }
}

/* Runs after a full-framebuffer present has drained, while `present_fb` still
 * holds exactly what that present was meant to send. An overlay or
 * interlace legitimately leaves the shadow out of step, so the audit
 * pauses under either and re-primes after: everything is marked dirty and
 * the next present resends it. */
static void
send_audit_scan_row(int y) {
    const gfx_color_t* fb_row = present_fb + ((size_t)y * GFX_WIDTH);
    const gfx_color_t* shadow_row = send_shadow + ((size_t)y * GFX_WIDTH);
    if (memcmp(fb_row, shadow_row, (size_t)GFX_WIDTH * sizeof(gfx_color_t)) == 0) {
        return;
    }
    for (int x = 0; x < GFX_WIDTH; x++) {
        if (fb_row[x] == shadow_row[x]) {
            continue;
        }
        if (send_audit_first_x < 0) {
            send_audit_first_x = x;
            send_audit_first_y = y;
        }
        send_audit_uncovered_px++;
        send_audit_uncovered_total++;
        /* Resent in full next time, so one gap is counted once. */
        send_audit_primed = false;
    }
}

static void
send_audit_log_if_due(void) {
    const int64_t now = timing_now_us();
    if (now < send_audit_log_at_us || (send_audit_uncovered_px == 0 && send_audit_copy_fault_px == 0)) {
        return;
    }
    send_audit_log_at_us = now + SEND_AUDIT_LOG_US;
    ESP_LOGW(
        TAG,
        "send audit: %lld px on the panel differ from present_fb (first at %d,%d), %lld px read back wrong from PSRAM",
        (long long)send_audit_uncovered_px, send_audit_first_x, send_audit_first_y,
        (long long)send_audit_copy_fault_px);
    send_audit_uncovered_px = 0;
    send_audit_copy_fault_px = 0;
    send_audit_first_x = -1;
    send_audit_first_y = -1;
}

void
send_audit_check(void) {
    if (send_shadow == NULL) {
        return;
    }
    if (overlay_any_on() || interlace_on) {
        send_audit_primed = false;
        return;
    }
    if (!send_audit_primed) {
        send_audit_primed = true;
        dirty_mark_all();
        return;
    }

    for (int y = 0; y < GFX_HEIGHT; y++) {
        send_audit_scan_row(y);
    }

    send_audit_log_if_due();
}

/* Green border on each leaf of `row` set in `leaves` inside the `height`
 * rows from `y0`; `buf` holds those rows starting at buffer row 0. */
static void
mark_leaf_borders(const uint16_t* leaves, int row, int y0, int height, gfx_color_t* buf) {
    const int n = dirty_leaf_rects(leaves, row, recti_from_edges(0, y0, GFX_WIDTH, y0 + height), leaf_rect_scratch,
                                   LEAF_RECTS_PER_ROW_MAX);
    for (int i = 0; i < n; i++) {
        const recti_t* r = &leaf_rect_scratch[i];
        gfx_color_t* at = buf + ((size_t)(r->y - y0) * GFX_WIDTH) + r->x;
        mark_rect_border(at, GFX_WIDTH, r->w, r->h, RGB565BE(0x00FF00));
    }
}

/* The expanded strip in `slot` is a disposable copy, so borders drawn here
 * never need restoring. The strip goes out whole, so the whole strip is
 * recorded. Must run before dirty_row_sent() clears the row's
 * leaf bits. */
void
mark_strip_overlay(gfx_color_t* slot, int row) {
    const int y = row * STRIP_HEIGHT;
    if (debug_overlay_on) {
        for (int col = 0; col < GRID_COLS; col++) {
            mark_rect_border(slot + (col * COL_WIDTH), GFX_WIDTH, COL_WIDTH, STRIP_HEIGHT, RGB565BE(0x00FFFF));
        }
    }
    if (leaf_overlay_on) {
        mark_leaf_borders(overlay_view()->leaves, row, y, STRIP_HEIGHT, slot);
    }
    overlay_note_bordered(recti_from_edges(0, y, GFX_WIDTH, y + STRIP_HEIGHT));
}

static void
save_overlay_borders(int y, int leaf_n) {
    if (debug_overlay_on) {
        for (int col = 0; col < GRID_COLS; col++) {
            gfx_color_t* cell = present_fb + ((size_t)y * GFX_WIDTH) + (col * COL_WIDTH);
            save_border(cell, GFX_WIDTH, COL_WIDTH, STRIP_HEIGHT, overlay_cell_save()[col]);
        }
    }
    for (int i = 0; i < leaf_n; i++) {
        const recti_t* r = &leaf_rect_scratch[i];
        gfx_color_t* at = present_fb + ((size_t)r->y * GFX_WIDTH) + r->x;
        save_border(at, GFX_WIDTH, r->w, r->h, overlay_leaf_save()[i]);
    }
}

static void
draw_overlay_borders(int y, int leaf_n) {
    if (debug_overlay_on) {
        for (int col = 0; col < GRID_COLS; col++) {
            gfx_color_t* cell = present_fb + ((size_t)y * GFX_WIDTH) + (col * COL_WIDTH);
            mark_rect_border(cell, GFX_WIDTH, COL_WIDTH, STRIP_HEIGHT, RGB565BE(0x00FFFF));
        }
    }
    for (int i = 0; i < leaf_n; i++) {
        const recti_t* r = &leaf_rect_scratch[i];
        gfx_color_t* at = present_fb + ((size_t)r->y * GFX_WIDTH) + r->x;
        mark_rect_border(at, GFX_WIDTH, r->w, r->h, RGB565BE(0x00FF00));
    }
}

/* Restore in the reverse order of saving. */
static void
restore_overlay_borders(int y, int leaf_n) {
    for (int i = leaf_n - 1; i >= 0; i--) {
        const recti_t* r = &leaf_rect_scratch[i];
        gfx_color_t* at = present_fb + ((size_t)r->y * GFX_WIDTH) + r->x;
        restore_border(at, GFX_WIDTH, r->w, r->h, overlay_leaf_save()[i]);
    }
    if (debug_overlay_on) {
        for (int col = GRID_COLS - 1; col >= 0; col--) {
            gfx_color_t* cell = present_fb + ((size_t)y * GFX_WIDTH) + (col * COL_WIDTH);
            restore_border(cell, GFX_WIDTH, COL_WIDTH, STRIP_HEIGHT, overlay_cell_save()[col]);
        }
    }
}

/* Sent and waited for at once, so the overlay's pixels are restored before
 * anything else draws. False when there is nothing to overlay: no grid
 * overlay and no dirty leaves in this strip. */
bool
send_row_with_overlays(int row, int y) {
    /* leaf_rect_scratch never NULL: gfx_set_leaf_overlay() allocates */
    int leaf_n = 0;
    if (leaf_overlay_on) {
        leaf_n = dirty_leaf_rects(overlay_view()->leaves, row, recti_from_edges(0, y, GFX_WIDTH, y + STRIP_HEIGHT),
                                  leaf_rect_scratch, LEAF_RECTS_PER_ROW_MAX);
    }
    if (!debug_overlay_on && leaf_n == 0) {
        return false;
    }

    /* Save phase: prevent overwriting shared pixels. */
    save_overlay_borders(y, leaf_n);
    draw_overlay_borders(y, leaf_n);
    if (send_fb_rows(y, y + STRIP_HEIGHT)) {
        xSemaphoreTake(strip_sent, portMAX_DELAY);
    }
    restore_overlay_borders(y, leaf_n);
    if (debug_overlay_on) {
        overlay_note_bordered(recti_from_edges(0, y, GFX_WIDTH, y + STRIP_HEIGHT));
    }
    for (int i = 0; i < leaf_n; i++) {
        overlay_note_bordered(leaf_rect_scratch[i]);
    }
    return true;
}

/* Borders `r` (panel pixels) in `slot`, which holds panel box `box` packed at
 * its own width, and records it for the next take-off. */
static void
outline_in_box(gfx_color_t* slot, recti_t box, recti_t r, gfx_color_t colour) {
    mark_rect_border(slot + ((size_t)(r.y - box.y) * box.w) + (r.x - box.x), box.w, r.w, r.h, colour);
    overlay_note_bordered(r);
}

/* Borders a gathered run packed into `slot` (`box` in panel pixels) from the
 * overlay view: yellow around each of the run's dirty cells, or one box
 * around them for a refined split; green around its leaves. Last present's
 * take-off can widen the box sent, never what is outlined. */
void
overlay_border_gathered(gfx_color_t* slot, recti_t box, int run_start, int run_end, bool refined, gfx_color_t border) {
    const overlay_view_t* view = overlay_view();
    const int row = box.y / STRIP_HEIGHT;
    if (debug_overlay_on) {
        recti_t split = recti_empty();
        for (int col = run_start; col < run_end; col++) {
            const recti_t cell = recti_intersect(view->cell[(row * GRID_COLS) + col], box);
            if (recti_is_empty(cell)) {
                continue;
            }
            if (refined) {
                split = recti_union(split, cell);
            } else {
                outline_in_box(slot, box, cell, border);
            }
        }
        if (!recti_is_empty(split)) {
            outline_in_box(slot, box, split, border);
        }
    }
    if (leaf_overlay_on) {
        const int n = dirty_leaf_rects(view->leaves, row, box, leaf_rect_scratch, LEAF_RECTS_PER_ROW_MAX);
        for (int i = 0; i < n; i++) {
            outline_in_box(slot, box, leaf_rect_scratch[i], RGB565BE(0x00FF00));
        }
    }
}

/* Per strip row, the union of the borders its last present drew; empty
 * when none. A border exists only in the bytes sent, never in `present_fb`, so
 * the panel keeps it until that box is sent again, and the dirty tracker never
 * resends what nothing changed in. Outlives the overlay toggles so switching
 * one off still cleans up. */
recti_t overlay_bordered[STRIP_COUNT];

void
overlay_note_bordered(recti_t box) {
    const int row = box.y / STRIP_HEIGHT;
    overlay_bordered[row] = recti_union(overlay_bordered[row], box);
}

/* Out of line: inlined, dirty_mark()'s frame would sit under every send
 * overlay_take_off_last_borders() makes. */
static __attribute__((noinline)) void
mark_box_dirty(recti_t box) {
    dirty_mark(box.x, box.y, box.w, box.h);
}

static void
take_overlay_view(void) {
    overlay_view_t* view = overlay_view();
    for (int idx = 0; idx < CELL_COUNT; idx++) {
        view->cell[idx] = (cell_dirty & (1U << idx))
                              ? recti_from_edges(cell_x0[idx], cell_y0[idx], cell_x1[idx], cell_y1[idx])
                              : recti_empty();
    }
    memcpy(view->leaves, leaf_dirty, sizeof view->leaves);
}

/* Before the dirty sends. A strip dirty again has last present's bordered box
 * marked dirty, so its one send with this present's borders also covers the
 * old ones; a quiet strip is resent clean. A strip interlace holds back keeps
 * its old borders until its own present. */
void
overlay_take_off_last_borders(bool (*send_rows)(int y0, int y1), int* queued) {
    if (overlay_any_on()) {
        take_overlay_view();
    }
    for (int row = 0; row < STRIP_COUNT; row++) {
        const recti_t box = overlay_bordered[row];
        if (recti_is_empty(box)) {
            continue;
        }
        if (dirty_row_is_dirty(row)) {
            mark_box_dirty(box);
        } else if (send_rows(row * STRIP_HEIGHT, (row + 1) * STRIP_HEIGHT)) {
            (*queued)++;
        }
        overlay_bordered[row] = recti_empty();
    }
}
#endif /* ESP_PLATFORM */

/* Cyan for the band that was sent, green for each leaf marked inside it. */
void
mark_band_overlay(gfx_color_t* buf, int row0, int height) {
    if (debug_overlay_on) {
        mark_rect_border(buf, GFX_WIDTH, GFX_WIDTH, height, RGB565BE(0x00FFFF));
    }
    if (leaf_overlay_on) {
        const int row_first = row0 / STRIP_HEIGHT;
        const int row_last = (row0 + height - 1) / STRIP_HEIGHT;
        for (int row = row_first; row <= row_last; row++) {
            mark_leaf_borders(leaf_dirty, row, row0, height, buf);
        }
    }
}
#endif
