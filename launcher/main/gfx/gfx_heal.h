/*
 * gfx_heal - which rows to send again, and how, so a region the panel link
 * received wrong gets corrected without the caller redrawing it.
 *
 * Past the panel's rated clock a send can land with stray pixels, and the
 * same window sent again fails the same way: the corruption follows the
 * shape of the transfer. So a heal never repeats the shape that was sent. It
 * sends full-width strips, GFX_HEAL_STRIP_ROWS tall, whose alignment moves
 * by GFX_HEAL_PHASE_STEP rows every present that heals, so the same rows are
 * cut differently each time. A pixel budget bounds each present; what does
 * not fit waits, and the next present resumes below the last strip sent.
 *
 * Pending rows are kept in GFX_HEAL_UNIT_ROWS units, one bit each, which is
 * finer than any strip and coarse enough for one uint64_t. Pure state, no
 * ESP-IDF, so a host suite drives it directly; gfx.c owns one instance.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#define GFX_HEAL_SCREEN_ROWS 448
#define GFX_HEAL_UNIT_ROWS   8
#define GFX_HEAL_UNITS       (GFX_HEAL_SCREEN_ROWS / GFX_HEAL_UNIT_ROWS)
#define GFX_HEAL_STRIP_ROWS  32
#define GFX_HEAL_PHASE_STEP  (3 * GFX_HEAL_UNIT_ROWS)

/* The most strips one plan can return: every unit's strip, disjoint. */
#define GFX_HEAL_MAX_STRIPS  (GFX_HEAL_SCREEN_ROWS / GFX_HEAL_STRIP_ROWS + 1)

_Static_assert(GFX_HEAL_UNITS <= 64, "pending units must fit one uint64_t");
_Static_assert(GFX_HEAL_STRIP_ROWS % GFX_HEAL_UNIT_ROWS == 0, "a strip must cover whole units");
_Static_assert(GFX_HEAL_UNIT_ROWS % 2 == 0, "strip edges must land on even panel rows");

typedef struct {
    uint64_t pending;
    int phase;       /* row offset of the strip lattice, a multiple of a unit */
    int cursor;      /* unit the next plan starts scanning from */
    int rolling_row; /* where the next rolling slice begins */
} gfx_heal_t;

typedef struct {
    int y0, y1;
} gfx_heal_strip_t;

static inline void
gfx_heal_reset(gfx_heal_t* h) {
    h->pending = 0;
    h->phase = 0;
    h->cursor = 0;
    h->rolling_row = 0;
}

/* Queues every unit [y0, y1) touches, clipped to the screen. */
static inline void
gfx_heal_queue_rows(gfx_heal_t* h, int y0, int y1) {
    y0 = y0 < 0 ? 0 : y0;
    y1 = y1 > GFX_HEAL_SCREEN_ROWS ? GFX_HEAL_SCREEN_ROWS : y1;
    if (y0 >= y1) {
        return;
    }
    for (int u = y0 / GFX_HEAL_UNIT_ROWS; u <= (y1 - 1) / GFX_HEAL_UNIT_ROWS; u++) {
        h->pending |= (uint64_t)1 << u;
    }
}

/* Queues the next `rows` of a sweep down the screen, wrapping at the bottom,
 * so a caller with no policy of its own heals everything in turn. */
static inline void
gfx_heal_queue_rolling(gfx_heal_t* h, int rows) {
    if (rows <= 0) {
        return;
    }
    rows = rows > GFX_HEAL_SCREEN_ROWS ? GFX_HEAL_SCREEN_ROWS : rows;
    const int end = h->rolling_row + rows;
    gfx_heal_queue_rows(h, h->rolling_row, end);
    if (end > GFX_HEAL_SCREEN_ROWS) {
        gfx_heal_queue_rows(h, 0, end - GFX_HEAL_SCREEN_ROWS);
    }
    h->rolling_row = end % GFX_HEAL_SCREEN_ROWS;
}

static inline bool
gfx_heal_pending(const gfx_heal_t* h) {
    return h->pending != 0;
}

/* The start of the lattice strip holding `row`, before clipping to the
 * screen - negative when `row` sits above the lattice's first full strip. */
static inline int
gfx_heal_strip_start(int row, int phase) {
    const int rel = row - phase;
    const int k = rel >= 0 ? rel / GFX_HEAL_STRIP_ROWS : -((-rel + GFX_HEAL_STRIP_ROWS - 1) / GFX_HEAL_STRIP_ROWS);
    return phase + k * GFX_HEAL_STRIP_ROWS;
}

/* Moves the strip lattice on by one heal - the same rows cut differently
 * next time, since the corruption follows the shape of the transfer.
 * gfx_heal_plan() calls this for its own strips; a band-mode send that
 * splits itself for healing (gfx.c) calls it directly, once a present, for
 * the same reason. */
static inline void
gfx_heal_advance_phase(gfx_heal_t* h) {
    h->phase = (h->phase + GFX_HEAL_PHASE_STEP) % GFX_HEAL_STRIP_ROWS;
}

/* Picks this present's strips, at most `budget_pixels` of `width`-wide rows,
 * and forgets the units they cover. Returns how many it wrote to `out`. */
static inline int
gfx_heal_plan(gfx_heal_t* h, int budget_pixels, int width, gfx_heal_strip_t* out, int max) {
    const int start = h->cursor;
    int n = 0;
    int used = 0;

    for (int i = 0; i < GFX_HEAL_UNITS && n < max; i++) {
        const int u = (start + i) % GFX_HEAL_UNITS;
        if (!((h->pending >> u) & 1u)) {
            continue;
        }
        int y0 = gfx_heal_strip_start(u * GFX_HEAL_UNIT_ROWS, h->phase);
        int y1 = y0 + GFX_HEAL_STRIP_ROWS;
        y0 = y0 < 0 ? 0 : y0;
        y1 = y1 > GFX_HEAL_SCREEN_ROWS ? GFX_HEAL_SCREEN_ROWS : y1;

        const int area = (y1 - y0) * width;
        if (used + area > budget_pixels) {
            break;
        }
        used += area;
        out[n].y0 = y0;
        out[n].y1 = y1;
        n++;
        for (int v = y0 / GFX_HEAL_UNIT_ROWS; v < y1 / GFX_HEAL_UNIT_ROWS; v++) {
            h->pending &= ~((uint64_t)1 << v);
        }
        h->cursor = (y1 / GFX_HEAL_UNIT_ROWS) % GFX_HEAL_UNITS;
    }

    if (n > 0) {
        gfx_heal_advance_phase(h);
    }
    return n;
}
