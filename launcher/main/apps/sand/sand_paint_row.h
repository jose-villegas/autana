/* sand_paint_row: the row walk and its carry and repaint state; sand_paint.h holds per-cell decisions. */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "gfx/gfx_indexed.h"
#include "material_palette.h"
#include "sand_limits.h"
#include "sand_paint.h"
#include "util/intmath.h"

#define SAND_PAINT_ROW_FLAG_SHINE          (1u << 0)
#define SAND_PAINT_ROW_FLAG_LIQUID         (1u << 1)
#define SAND_PAINT_ROW_FLAG_CULLET         (1u << 2)
#define SAND_PAINT_ROW_FLAG_GLASS          (1u << 3)
#define SAND_PAINT_ROW_FLAG_WOOD_LEAF      (1u << 4)

#define SAND_PAINT_FOAM_BLOB_SHIFT         3
#define SAND_PAINT_SHINE_PERIOD            64

#define SAND_PAINT_WOOD_LEAF_SLOTS_CHECKED 5u

#define SAND_PAINT_NO_ROW                  (-2)

/* Callers read row_flags, row_flag_x0/x1 and row_changed_x0/x1 after painting. */
typedef struct {
    unsigned local_depth_scale_q8;
    bool local_depth_vertical_dominant;
    bool local_depth_v_reverse;
    bool local_depth_h_reverse;
    unsigned local_depth_ax;
    unsigned local_depth_ay;

    bool local_depth_vertical_dominant_prev;
    bool local_depth_v_reverse_prev;
    bool local_depth_h_reverse_prev;

    uint8_t local_depth_rows[2][GRID_W_MAX];
    unsigned local_depth_cur_index;
    int local_depth_prev_cy;
    uint8_t local_depth_top_row[GRID_W_MAX];

    uint8_t row_flags[GRID_H_MAX];
    uint16_t row_flag_x0[GRID_H_MAX];
    uint16_t row_flag_x1[GRID_H_MAX];
    uint16_t row_changed_x0[GRID_H_MAX];
    uint16_t row_changed_x1[GRID_H_MAX];
} sand_paint_row_state_t;

/* Values refreshed by the frame update before painting. */
typedef struct {
    int shine_ux_q8;
    int shine_uy_q8;
    int shine_offset;

    int wood_leaf_wind_sign;
    uint32_t wood_leaf_time_ms;
    int wood_leaf_wind_ux_q8;
    int wood_leaf_wind_uy_q8;
    int8_t wood_leaf_top5[5][2];

    gfx_indexed_repaint_kind_t repaint_kind;
    const uint8_t* repaint_class_table;
    const gfx_color_t* repaint_cell_table;
} sand_paint_frame_t;

static inline void
sand_paint_row_state_init(sand_paint_row_state_t* s) {
    *s = (sand_paint_row_state_t){0};
    s->local_depth_prev_cy = SAND_PAINT_NO_ROW;
}

static inline void
sand_paint_update_local_depth_gravity(sand_paint_row_state_t* s, int gx, int gy, int grid_w, int grid_h) {
    const int ax = im_abs(gx), ay = im_abs(gy);

    const int len = im_len(gx, gy);
    const bool new_vertical_dominant = (ay >= ax);
    const unsigned dom_axis = new_vertical_dominant ? (unsigned)ay : (unsigned)ax;
    s->local_depth_scale_q8 = (dom_axis != 0u) ? (256u * (unsigned)len) / dom_axis : 256u;
    s->local_depth_ax = (unsigned)ax;
    s->local_depth_ay = (unsigned)ay;

    const bool new_v_reverse = (gy < 0);
    const bool new_h_reverse = (gx < 0);

    /* The gate below is exact arithmetic, not a deadband - each condition is
     * a statement about whether the flipped flag can change a number this
     * grid's walk actually computes, derived from the Bresenham arithmetic
     * itself; a regime flip is never gated, since it always changes what the
     * array slot means regardless of magnitude. */
    const bool drift_observable = ((long)grid_h * (long)ax >= (long)ay);
    const bool cross_row_observable = ((long)grid_w * (long)ay >= (long)ax);
    const bool v_reverse_matters =
        (new_v_reverse != s->local_depth_v_reverse_prev) && (new_vertical_dominant || cross_row_observable);
    const bool h_reverse_matters =
        (new_h_reverse != s->local_depth_h_reverse_prev) && (!new_vertical_dominant || drift_observable);

    if (new_vertical_dominant != s->local_depth_vertical_dominant_prev || v_reverse_matters || h_reverse_matters) {
        for (int cx = 0; cx < grid_w; cx++) {
            s->local_depth_rows[s->local_depth_cur_index][cx] = 0u;
            s->local_depth_rows[s->local_depth_cur_index ^ 1u][cx] = 0u;
            s->local_depth_top_row[cx] = 255u;
        }
        s->local_depth_prev_cy = SAND_PAINT_NO_ROW;
        s->local_depth_vertical_dominant_prev = new_vertical_dominant;
        s->local_depth_v_reverse_prev = new_v_reverse;
        s->local_depth_h_reverse_prev = new_h_reverse;
    }

    s->local_depth_vertical_dominant = new_vertical_dominant;
    s->local_depth_v_reverse = new_v_reverse;
    s->local_depth_h_reverse = new_h_reverse;
}

static inline void
sp_note_row_flag_x(sand_paint_row_state_t* s, int cy, int cx) {
    if (cx < s->row_flag_x0[cy]) {
        s->row_flag_x0[cy] = (uint16_t)cx;
    }
    if (cx + 1 > s->row_flag_x1[cy]) {
        s->row_flag_x1[cy] = (uint16_t)(cx + 1);
    }
}

static inline void
sp_note_row_change_x(sand_paint_row_state_t* s, int cy, int cx) {
    if (cx < s->row_changed_x0[cy]) {
        s->row_changed_x0[cy] = (uint16_t)cx;
    }
    if (cx + 1 > s->row_changed_x1[cy]) {
        s->row_changed_x1[cy] = (uint16_t)(cx + 1);
    }
}

/* One row's walk through the local-depth state, set up once per row by
 * sand_paint_row_n() and advanced cell by cell by sp_local_depth_count_at(). */
typedef struct {
    const uint8_t* toward_surface;
    bool chain_ok;
    int hdir;
    int row_step;
    int herr;
    int ysign;
} sp_local_depth_walk_t;

/* Dirty rows can be skipped, so the row offset depends on cy rather than
 * an accumulator. The difference of consecutive cumulative Bresenham
 * steps gives the offset without carrying state across rows. */
static inline __attribute__((always_inline)) int
sp_local_depth_row_step_at(const sand_paint_row_state_t* s, int cy, int vdir, int grid_h) {
    if (!s->local_depth_vertical_dominant || s->local_depth_ay == 0u) {
        return 0;
    }
    const int xsign = s->local_depth_h_reverse ? 1 : -1;
    const int n = (vdir > 0) ? cy : (grid_h - 1 - cy);
    const int cum_n = (int)(((long)(n) * (long)s->local_depth_ax) / (long)s->local_depth_ay);
    const int cum_n1 = (int)(((long)(n + 1) * (long)s->local_depth_ax) / (long)s->local_depth_ay);
    return xsign * (cum_n1 - cum_n);
}

static inline __attribute__((always_inline)) int
sp_local_depth_cross_step(bool vertical_dominant, unsigned ax, unsigned ay, sp_local_depth_walk_t* walk) {
    if (vertical_dominant) {
        return 0;
    }
    walk->herr += (int)ay;
    if (ax > 0u && walk->herr >= (int)ax) {
        walk->herr -= (int)ax;
        return walk->ysign;
    }
    return 0;
}

static inline __attribute__((always_inline)) unsigned
sp_local_depth_count_at(sand_paint_row_state_t* s, sp_local_depth_walk_t* walk, const uint8_t* row, int cx, int cy,
                        bool here_liquid, int grid_w, bool vertical_dominant, unsigned ax, unsigned ay) {
    const int step = sp_local_depth_cross_step(vertical_dominant, ax, ay, walk);
    const int qx = vertical_dominant ? (cx + walk->row_step) : (cx - walk->hdir);
    const bool qx_ok = (qx >= 0 && qx < grid_w);
    const bool cross_row = vertical_dominant || (step != 0);
    const uint8_t* src_ptr = cross_row ? walk->toward_surface : row;
    const uint8_t* src_arr =
        cross_row ? s->local_depth_rows[s->local_depth_cur_index ^ 1u] : s->local_depth_rows[s->local_depth_cur_index];

    const bool same_material =
        here_liquid && qx_ok && src_ptr != NULL && (CELL_MATERIAL(src_ptr[qx]) == CELL_MATERIAL(row[cx]));
    const unsigned src_count = qx_ok ? src_arr[qx] : 0u;

    const unsigned count = here_liquid
                               ? sand_paint_depth_count(s->local_depth_top_row, vertical_dominant, same_material,
                                                        !cross_row || walk->chain_ok, src_count, cx, cy)
                               : 0u;
    s->local_depth_rows[s->local_depth_cur_index][cx] = (uint8_t)count;
    return count;
}

/* The `depth` material_colours() takes: a root's neighbour count, a leaf
 * wave for canopy cells, else the liquid depth `count` projects to. */
static inline __attribute__((always_inline)) unsigned
sp_cell_shading_depth(const sand_paint_frame_t* pf, const uint8_t* above, const uint8_t* row, const uint8_t* below,
                      int cx, int cy, unsigned hash, bool leaf_shading, unsigned count, int grid_w, unsigned scale_q8) {
    const unsigned depth_raw = (count * scale_q8) >> 8;
    const unsigned depth_liquid = depth_raw < MATERIAL_LIQUID_DEPTH_BAND ? depth_raw : MATERIAL_LIQUID_DEPTH_BAND;

    /* Projected onto the wind axis, not raw `cx` - a grid column is not a
     * screen-relative direction once rotated. The wave's fraction below is
     * offset by 1 since its own trough is legitimately 0, which must still
     * select wood_colours()'s tint branch rather than the untinted look. */
    const int wood_leaf_wind_pos =
        pf->wood_leaf_wind_sign * ((cx * pf->wood_leaf_wind_ux_q8 + cy * pf->wood_leaf_wind_uy_q8) >> 8);

    return (row[cx] == MATX(MATX_ROOT))
               ? material_root_neighbours(above, row, below, cx, grid_w)
               : (leaf_shading ? material_wood_leaf_wave(pf->wood_leaf_time_ms, wood_leaf_wind_pos, grid_w, hash) + 1u
                               : depth_liquid);
}

static inline __attribute__((always_inline)) void
sp_note_row_flag(sand_paint_row_state_t* s, int cy, int cx, unsigned flag) {
    s->row_flags[cy] |= flag;
    sp_note_row_flag_x(s, cy, cx);
}

static inline __attribute__((always_inline)) void
sp_note_cell_row_flags(sand_paint_row_state_t* s, int cy, int cx, uint8_t cell, bool here_liquid, bool leaf_shading) {
    const unsigned cullet_first = MAT_SAND * MATERIAL_VARIANTS + SAND_CULLET_BASE;

    if (here_liquid) {
        sp_note_row_flag(s, cy, cx, SAND_PAINT_ROW_FLAG_LIQUID);
    }
    if (leaf_shading) {
        sp_note_row_flag(s, cy, cx, SAND_PAINT_ROW_FLAG_WOOD_LEAF);
    }
    if ((unsigned)(cell - cullet_first) < SAND_CULLET_SHADES) {
        sp_note_row_flag(s, cy, cx, SAND_PAINT_ROW_FLAG_CULLET);
    }
    if (CELL_MATERIAL(cell) == MAT_GLASS) {
        sp_note_row_flag(s, cy, cx, SAND_PAINT_ROW_FLAG_GLASS);
    }
}

/* MATERIAL_HATCHED's diagonal cannot survive one index per cell, but
 * whether THIS cell falls on the band still can: the same shine line,
 * sampled once at the cell's own centre instead of per pixel. A cell the
 * line crosses takes col[2]'s own index (already in the study's sweep -
 * see material_palette256_index()'s own comment) instead of col[0]'s. */
static inline __attribute__((always_inline)) void
sp_paint_indexed_cell(sand_paint_row_state_t* s, const sand_paint_frame_t* pf, uint8_t* index_row, int cx, int cy,
                      int n, material_pattern_t pat, const gfx_color_t col[3], bool force_full) {
    gfx_color_t shade = col[0];
    if (pat == MATERIAL_HATCHED) {
        const int shine_q8 = (cx * n + n / 2) * pf->shine_ux_q8 + (cy * n + n / 2) * pf->shine_uy_q8;
        const int along = ((shine_q8 >> 8) + pf->shine_offset) & (SAND_PAINT_SHINE_PERIOD - 1);
        shade = (along < n) ? col[2] : col[0];
    }

    /* Compares against what this cell already holds - the index image is
     * never cleared between frames (only on a fresh indexed entry, see
     * apply_gfx_enter_indexed()), so it IS last frame's sent value, at no
     * extra storage. */
    const uint8_t new_idx = (uint8_t)material_palette256_index(shade);
    const uint8_t old_idx = index_row[cx];
    if (gfx_indexed_cell_repaint(pf->repaint_kind, pf->repaint_class_table, pf->repaint_cell_table, force_full, old_idx,
                                 new_idx, cx, cy)) {
        index_row[cx] = new_idx;
        sp_note_row_change_x(s, cy, cx);
    }
}

static inline __attribute__((always_inline)) void
sp_fill_cell_solid(gfx_color_t* p, int n, gfx_color_t c) {
    for (int dy = 0; dy < n; dy++) {
        for (int dx = 0; dx < n; dx++) {
            p[dy * GFX_WIDTH + dx] = c;
        }
    }
}

static inline __attribute__((always_inline)) void
sp_fill_cell_hatched(const sand_paint_frame_t* pf, gfx_color_t* p, int cx, int cy, int n, const gfx_color_t col[3]) {
    const int shine_base_q8 = (cx * n) * pf->shine_ux_q8 + (cy * n) * pf->shine_uy_q8;

    for (int dy = 0; dy < n; dy++) {
        for (int dx = 0; dx < n; dx++) {

            const int shine_q8 = shine_base_q8 + dx * pf->shine_ux_q8 + dy * pf->shine_uy_q8;
            const int along = ((shine_q8 >> 8) + pf->shine_offset) & (SAND_PAINT_SHINE_PERIOD - 1);

            p[dy * GFX_WIDTH + dx] = (along < n) ? col[2] : col[0];
        }
    }
}

/* Cross-row carry requires the adjacent row; see Shading-and-Colour.md, "Local depth". */
static inline __attribute__((always_inline)) sp_local_depth_walk_t
sp_local_depth_walk_begin(const sand_paint_row_state_t* s, int cy, const uint8_t* above, const uint8_t* below,
                          int grid_h) {
    const int local_depth_vdir = s->local_depth_v_reverse ? -1 : 1;
    return (sp_local_depth_walk_t){
        .toward_surface = s->local_depth_v_reverse ? below : above,
        .chain_ok = (s->local_depth_prev_cy == cy - local_depth_vdir),
        .hdir = s->local_depth_h_reverse ? -1 : 1,
        .row_step = sp_local_depth_row_step_at(s, cy, local_depth_vdir, grid_h),
        .herr = 0,
        .ysign = s->local_depth_v_reverse ? 1 : -1,
    };
}

static inline __attribute__((always_inline)) unsigned
sp_cell_grain_hash(uint8_t cell, int cx, int cy) {
    return CELL_MATERIAL(cell) == MAT_WATER
               ? material_grain_hash(cx >> SAND_PAINT_FOAM_BLOB_SHIFT, cy >> SAND_PAINT_FOAM_BLOB_SHIFT)
               : material_grain_hash(cx, cy);
}

/* Every leaf cell rides the same wave unconditionally - no adjacency
 * check needed, unlike wood, since being leaf already means being part
 * of the canopy. */
static inline __attribute__((always_inline)) bool
sp_cell_leaf_shaded(const sand_paint_frame_t* pf, const uint8_t* above, const uint8_t* row, const uint8_t* below,
                    int cx, unsigned hash, int grid_w) {
    const bool wood_near_leaf = row[cx] == CELL_MAKE(MAT_WOOD, 0)
                                && material_wood_near_leaf(above, row, below, cx, grid_w, pf->wood_leaf_top5, hash,
                                                           SAND_PAINT_WOOD_LEAF_SLOTS_CHECKED);
    return wood_near_leaf || row[cx] == MATX(MATX_LEAF);
}

/* `out` is the RGB565 row the cell's n x n block starts in, unused when
 * `index_row` is non-NULL. */
static inline __attribute__((always_inline)) void
sp_paint_cell_output(sand_paint_row_state_t* s, const sand_paint_frame_t* pf, gfx_color_t* out, uint8_t* index_row,
                     int cx, int cy, int n, material_pattern_t pat, const gfx_color_t col[3], bool force_full) {
    if (index_row != NULL) {
        sp_paint_indexed_cell(s, pf, index_row, cx, cy, n, pat, col, force_full);
    } else if (pat != MATERIAL_HATCHED) {
        sp_fill_cell_solid(out + cx * n, n, col[0]);
    } else {
        sp_fill_cell_hatched(pf, out + cx * n, cx, cy, n, col);
    }
}

/* A NULL index_row selects RGB565 output; otherwise each in-span cell
 * writes one palette index. See Shading-and-Colour.md for shine sampling. */
static inline void
sand_paint_row_n(sand_paint_row_state_t* s, const sand_paint_frame_t* pf, gfx_color_t* fb, uint8_t* index_row, int cy,
                 const uint8_t* row, int n, int grid_w, int grid_h, int wx0, int wx1, bool force_full) {
    gfx_color_t* out = index_row == NULL ? fb + (cy * n) * GFX_WIDTH : NULL;
    s->row_flags[cy] = 0;
    s->row_flag_x0[cy] = (uint16_t)grid_w;
    s->row_flag_x1[cy] = 0;
    s->row_changed_x0[cy] = (uint16_t)grid_w;
    s->row_changed_x1[cy] = 0;

    const uint8_t* above = (cy > 0) ? row - grid_w : NULL;
    const uint8_t* below = (cy < grid_h - 1) ? row + grid_w : NULL;

    sp_local_depth_walk_t walk = sp_local_depth_walk_begin(s, cy, above, below, grid_h);

    const bool vertical_dominant = s->local_depth_vertical_dominant;
    const unsigned scale_q8 = s->local_depth_scale_q8;
    const unsigned ax = s->local_depth_ax;
    const unsigned ay = s->local_depth_ay;
    const bool h_reverse = s->local_depth_h_reverse;
    const int cx_first = h_reverse ? grid_w - 1 : 0;
    const int cx_step = h_reverse ? -1 : 1;

    for (int cx_i = 0; cx_i < grid_w; cx_i++) {
        const int cx = cx_first + cx_i * cx_step;

        const unsigned mask = sand_paint_edge_mask(above, row, below, cx, grid_w);

        const unsigned hash = sp_cell_grain_hash(row[cx], cx, cy);

        const bool here_liquid = material_of(row[cx])->kind == KIND_LIQUID;
        const unsigned count =
            sp_local_depth_count_at(s, &walk, row, cx, cy, here_liquid, grid_w, vertical_dominant, ax, ay);

        const bool leaf_shading = sp_cell_leaf_shaded(pf, above, row, below, cx, hash, grid_w);

        const unsigned depth =
            sp_cell_shading_depth(pf, above, row, below, cx, cy, hash, leaf_shading, count, grid_w, scale_q8);

        sp_note_cell_row_flags(s, cy, cx, row[cx], here_liquid, leaf_shading);

        gfx_color_t col[3];
        const material_pattern_t pat = material_colours(row[cx], hash, mask, depth, col);

        if (pat == MATERIAL_HATCHED) {
            sp_note_row_flag(s, cy, cx, SAND_PAINT_ROW_FLAG_SHINE);
        }

        /* State above (local depth, row_flags, hash) runs the full row
         * regardless - only pixels reaching the framebuffer are bounded to
         * [wx0,wx1). A cx outside it has provably unchanged output: every
         * mark site already widens for the reach its own output depends on. */
        const bool in_span = cx >= wx0 && cx < wx1;
        if (in_span) {
            sp_paint_cell_output(s, pf, out, index_row, cx, cy, n, pat, col, force_full);
        }
    }

    s->local_depth_cur_index ^= 1u;

    s->local_depth_prev_cy = cy;
}
