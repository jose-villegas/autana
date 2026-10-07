/*
 * gfx_shared_state: a snapshot of the state gfx_dirty.h, gfx_fb_guard.h,
 * gfx_full_redraw.h and gfx_present_guard.h share with the running
 * firmware. A suite that drives those headers directly saves it first and
 * restores it last, so a device run leaves the shell's own state as it was.
 */
#pragma once

#include <string.h>

#include "gfx/present/gfx_dirty.h"
#include "gfx/present/gfx_fb_guard.h"
#include "gfx/present/gfx_full_redraw.h"
#include "gfx/present/gfx_present_guard.h"

typedef struct {
    uint32_t cell_dirty;
    bool all_dirty;
    int cell_x0[CELL_COUNT], cell_x1[CELL_COUNT], cell_y0[CELL_COUNT], cell_y1[CELL_COUNT];
    uint16_t leaf_dirty[STRIP_COUNT * LEAF_SUB];
    bool fb_available;
    bool band_force_all;
    bool full_redraw_latched;
    bool present_in_flight;
} gfx_shared_state_t;

static inline void
gfx_shared_state_save(gfx_shared_state_t* s) {
    s->cell_dirty = cell_dirty;
    s->all_dirty = all_dirty;
    memcpy(s->cell_x0, cell_x0, sizeof cell_x0);
    memcpy(s->cell_x1, cell_x1, sizeof cell_x1);
    memcpy(s->cell_y0, cell_y0, sizeof cell_y0);
    memcpy(s->cell_y1, cell_y1, sizeof cell_y1);
    memcpy(s->leaf_dirty, leaf_dirty, sizeof leaf_dirty);
    s->fb_available = gfx_fb_guard_available;
    s->band_force_all = gfx_band_force_all_dirty;
    s->full_redraw_latched = gfx_full_redraw_latched;
    s->present_in_flight = gfx_present_guard_in_flight;
}

static inline void
gfx_shared_state_restore(const gfx_shared_state_t* s) {
    cell_dirty = s->cell_dirty;
    all_dirty = s->all_dirty;
    memcpy(cell_x0, s->cell_x0, sizeof cell_x0);
    memcpy(cell_x1, s->cell_x1, sizeof cell_x1);
    memcpy(cell_y0, s->cell_y0, sizeof cell_y0);
    memcpy(cell_y1, s->cell_y1, sizeof cell_y1);
    memcpy(leaf_dirty, s->leaf_dirty, sizeof leaf_dirty);
    gfx_fb_guard_available = s->fb_available;
    gfx_band_force_all_dirty = s->band_force_all;
    gfx_full_redraw_latched = s->full_redraw_latched;
    gfx_present_guard_in_flight = s->present_in_flight;
}
