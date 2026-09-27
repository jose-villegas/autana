/* sand_paint: per-cell decisions; sand_paint_row.h owns the row walk and its state. */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "material_palette.h"
#include "util/intmath.h"

/* Unlike the two-walk design's own ceiling (34, raised above
 * MATERIAL_LIQUID_DEPTH_BAND's 24), this walk needs no raise: that design's
 * weight (component/len) was always <= 256, so a clamped count could
 * project BELOW the band; this walk's scale (len/dominant_axis) is always
 * >= 256, so a clamped count already projects AT LEAST the band at every
 * angle. Verified host-side against
 * test_a_saturated_liquid_body_reads_the_same_shade_at_every_tilt_angle. */
#define LOCAL_DEPTH_COUNT_CEILING MATERIAL_LIQUID_DEPTH_BAND

static inline __attribute__((always_inline)) unsigned
sand_paint_cardinal_edges(const uint8_t* above, const uint8_t* row, const uint8_t* below, int cx, int w) {
    return ((cx > 0 && CELL_IS_EMPTY(row[cx - 1])) ? MATERIAL_EDGE_LEFT : 0u)
           | ((cx < w - 1 && CELL_IS_EMPTY(row[cx + 1])) ? MATERIAL_EDGE_RIGHT : 0u)
           | ((above != NULL && CELL_IS_EMPTY(above[cx])) ? MATERIAL_EDGE_UP : 0u)
           | ((below != NULL && CELL_IS_EMPTY(below[cx])) ? MATERIAL_EDGE_DOWN : 0u);
}

static inline __attribute__((always_inline)) unsigned
sand_paint_diagonal_edges(const uint8_t* above, const uint8_t* below, int cx, int w) {
    return ((cx > 0 && above != NULL && CELL_IS_EMPTY(above[cx - 1])) ? MATERIAL_EDGE_UP_LEFT : 0u)
           | ((cx < w - 1 && above != NULL && CELL_IS_EMPTY(above[cx + 1])) ? MATERIAL_EDGE_UP_RIGHT : 0u)
           | ((cx > 0 && below != NULL && CELL_IS_EMPTY(below[cx - 1])) ? MATERIAL_EDGE_DOWN_LEFT : 0u)
           | ((cx < w - 1 && below != NULL && CELL_IS_EMPTY(below[cx + 1])) ? MATERIAL_EDGE_DOWN_RIGHT : 0u);
}

/* The empty-neighbour mask for cell `cx` of a row `w` cells wide; `above` or
 * `below` is NULL at the grid's edge. Only water that already has an empty
 * side also reads its diagonals. */
static inline __attribute__((always_inline)) unsigned
sand_paint_edge_mask(const uint8_t* above, const uint8_t* row, const uint8_t* below, int cx, int w) {
    unsigned mask = sand_paint_cardinal_edges(above, row, below, cx, w);
    if ((mask & MATERIAL_EDGE_CARDINAL) != 0 && CELL_MATERIAL(row[cx]) == MAT_WATER) {
        mask |= sand_paint_diagonal_edges(above, below, cx, w);
    }
    return mask;
}

static inline __attribute__((always_inline)) unsigned
sand_paint_depth_next(unsigned carry) {
    return carry < LOCAL_DEPTH_COUNT_CEILING ? carry + 1u : LOCAL_DEPTH_COUNT_CEILING;
}

/* A liquid cell's local-depth count. `same_material` climbs from the
 * surface-ward source's `src_count`; otherwise the column's `top_row` entry
 * decides whether it already committed to another source this pass (0) or
 * restarts here, carrying `src_count` only while `carry_ok`. Vertical- and
 * horizontal-dominant gravity keep `top_row` in different conventions. */
static inline __attribute__((always_inline)) unsigned
sand_paint_depth_count(uint8_t* top_row, bool vertical_dominant, bool same_material, bool carry_ok, unsigned src_count,
                       int cx, int cy) {
    if (same_material) {
        if (!vertical_dominant) {
            top_row[cx] = 255u;
        }
        return sand_paint_depth_next(src_count);
    }
    const bool committed = vertical_dominant ? (top_row[cx] == (uint8_t)cy) : (top_row[cx] != 255u);
    if (committed) {
        return 0u;
    }
    top_row[cx] = vertical_dominant ? (uint8_t)cy : 0u;
    return sand_paint_depth_next(carry_ok ? src_count : 0u);
}

/* One send range [send_x0, send_x1) clipped to the painted span [wx0, wx1),
 * and in indexed modes further to the changed span [changed_x0, changed_x1).
 * False when nothing is left to send. */
static inline __attribute__((always_inline)) bool
sand_paint_clip_send(int send_x0, int send_x1, int wx0, int wx1, bool indexed, int changed_x0, int changed_x1,
                     int* out_x0, int* out_x1) {
    int sx0 = im_max(send_x0, wx0);
    int sx1 = im_min(send_x1, wx1);
    if (indexed) {
        sx0 = im_max(sx0, changed_x0);
        sx1 = im_min(sx1, changed_x1);
    }
    *out_x0 = sx0;
    *out_x1 = sx1;
    return sx0 < sx1;
}
