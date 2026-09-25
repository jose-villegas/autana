/*
 * sand_paint_row - paint_row_n() and the local-depth walk it carries row to
 * row, split out of app_sand.c so a host suite can call the exact code the
 * firmware inlines. Everything read here arrives as an argument, a
 * sand_paint_row_state_t the caller owns (local depth's cross-row carry,
 * the per-row dirty spans), or a sand_paint_frame_t the caller refreshes
 * once per frame (shine sweep, wood/leaf wind, indexed repaint dispatch).
 *
 * Header of `static inline` functions, not a .c file: app_sand.c includes
 * this directly, so paint_row_n() stays inlined into the same translation
 * unit it always was, at the same call site, with the same
 * `always_inline`/loop shape - not a new cross-TU call.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "gfx/gfx_indexed.h"
#include "material_palette.h"
#include "sand_limits.h"
#include "sand_paint.h"
#include "util/intmath.h"

#define SAND_PAINT_ROW_FLAG_SHINE     (1u << 0)
#define SAND_PAINT_ROW_FLAG_LIQUID    (1u << 1)
#define SAND_PAINT_ROW_FLAG_CULLET    (1u << 2)
#define SAND_PAINT_ROW_FLAG_GLASS     (1u << 3)
#define SAND_PAINT_ROW_FLAG_WOOD_LEAF (1u << 4)

#define FOAM_BLOB_SHIFT               3

#define WOOD_LEAF_SLOTS_CHECKED       5u

#define LOCAL_DEPTH_NO_ROW            (-2)

/* Local depth's cross-row carry and the per-row dirty spans paint_row_n()
 * writes - see Shading-and-Colour.md, "Local depth". One instance, owned by
 * the caller and passed by pointer; `local_depth_cur_row`/`local_depth_prev_row`
 * point at two of the caller's own GRID_W_MAX rows (a plain ping-pong, not
 * owned here so the caller's static initializer stays the obvious one). */
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

    uint8_t* local_depth_cur_row;
    uint8_t* local_depth_prev_row;
    int local_depth_prev_cy;
    uint8_t local_depth_top_row[GRID_W_MAX];

    uint8_t row_flags[GRID_H_MAX];
    uint16_t row_flag_x0[GRID_H_MAX];
    uint16_t row_flag_x1[GRID_H_MAX];
    uint16_t row_changed_x0[GRID_H_MAX];
    uint16_t row_changed_x1[GRID_H_MAX];
} sand_paint_row_state_t;

/* Everything paint_row_n() reads that changes once a frame, never once a
 * cell: the travelling shine, the wood/leaf wind, and which indexed-mode
 * repaint dispatch is active. Built fresh by the caller wherever it used to
 * read these off its own globals directly - once per row-draw call, not
 * once per cell. */
typedef struct {
    int shine_ux_q8;
    int shine_uy_q8;
    int shine_offset;
    int shine_period; /* power of two - see the mask below */

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
update_local_depth_gravity(sand_paint_row_state_t* s, int gx, int gy, int grid_w, int grid_h) {
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
            s->local_depth_cur_row[cx] = 0u;
            s->local_depth_prev_row[cx] = 0u;
            s->local_depth_top_row[cx] = 255u;
        }
        s->local_depth_prev_cy = LOCAL_DEPTH_NO_ROW;
        s->local_depth_vertical_dominant_prev = new_vertical_dominant;
        s->local_depth_v_reverse_prev = new_v_reverse;
        s->local_depth_h_reverse_prev = new_h_reverse;
    }

    s->local_depth_vertical_dominant = new_vertical_dominant;
    s->local_depth_v_reverse = new_v_reverse;
    s->local_depth_h_reverse = new_h_reverse;
}

static inline void
note_row_flag_x(sand_paint_row_state_t* s, int cy, int cx) {
    if (cx < s->row_flag_x0[cy]) {
        s->row_flag_x0[cy] = (uint16_t)cx;
    }
    if (cx + 1 > s->row_flag_x1[cy]) {
        s->row_flag_x1[cy] = (uint16_t)(cx + 1);
    }
}

static inline void
note_row_change_x(sand_paint_row_state_t* s, int cy, int cx) {
    if (cx < s->row_changed_x0[cy]) {
        s->row_changed_x0[cy] = (uint16_t)cx;
    }
    if (cx + 1 > s->row_changed_x1[cy]) {
        s->row_changed_x1[cy] = (uint16_t)(cx + 1);
    }
}

/* paint_row_n()'s one per-cell decision - repaint_kind and its table were
 * resolved once at apply_gfx_enter_indexed() time, not re-derived here:
 * a force_full check, an index compare, one lookup, no switch per cell. */
static inline bool
sand_indexed_cell_needs_repaint(const sand_paint_frame_t* pf, bool force_full, uint8_t old_idx, uint8_t new_idx, int cx,
                                int cy) {
    return gfx_indexed_cell_repaint(pf->repaint_kind, pf->repaint_class_table, pf->repaint_cell_table, force_full,
                                    old_idx, new_idx, cx, cy);
}

static inline __attribute__((always_inline)) unsigned
cell_edge_mask(const uint8_t* above, const uint8_t* row, const uint8_t* below, int cx, int grid_w) {
    return sand_paint_edge_mask(above, row, below, cx, grid_w);
}

/* One row's walk through the local-depth state, set up once per row by
 * paint_row_n() and advanced cell by cell by local_depth_count_at(). */
typedef struct {
    const uint8_t* toward_surface;
    bool chain_ok;
    int hdir;
    int row_step;
    int herr;
    int ysign;
} local_depth_walk_t;

/* THE ROW OFFSET, WITHOUT AN ACCUMULATOR: computed from `cy` alone, not
 * a running Bresenham accumulator carried across paint_row_n() calls -
 * that function only runs for DIRTY rows, so an accumulator would
 * silently skip gaps and drift out of sync. `cum(n) - cum(n - 1)`
 * (cum(n) = floor(n*minor/dominant)) gives the same drift a real march
 * would, verified by hand, with no memory of prior rows needed.
 * Meaningless (0) when horizontal-dominant or gravity has no
 * direction. */
static inline __attribute__((always_inline)) int
local_depth_row_step_at(const sand_paint_row_state_t* s, int cy, int vdir, int grid_h) {
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
local_depth_cross_step(const sand_paint_row_state_t* s, local_depth_walk_t* walk) {
    if (s->local_depth_vertical_dominant) {
        return 0;
    }
    walk->herr += (int)s->local_depth_ay;
    if (s->local_depth_ax > 0u && walk->herr >= (int)s->local_depth_ax) {
        walk->herr -= (int)s->local_depth_ax;
        return walk->ysign;
    }
    return 0;
}

static inline __attribute__((always_inline)) unsigned
local_depth_liquid_count(sand_paint_row_state_t* s, bool same_material, bool carry_ok, unsigned src_count, int cx,
                         int cy) {
    return sand_paint_depth_count(s->local_depth_top_row, s->local_depth_vertical_dominant, same_material, carry_ok,
                                  src_count, cx, cy);
}

static inline __attribute__((always_inline)) unsigned
local_depth_count_at(sand_paint_row_state_t* s, local_depth_walk_t* walk, const uint8_t* row, int cx, int cy,
                     bool here_liquid, int grid_w) {
    const int step = local_depth_cross_step(s, walk);
    const int qx = s->local_depth_vertical_dominant ? (cx + walk->row_step) : (cx - walk->hdir);
    const bool qx_ok = (qx >= 0 && qx < grid_w);
    const bool cross_row = s->local_depth_vertical_dominant || (step != 0);
    const uint8_t* src_ptr = cross_row ? walk->toward_surface : row;
    const uint8_t* src_arr = cross_row ? s->local_depth_prev_row : s->local_depth_cur_row;

    const bool same_material =
        here_liquid && qx_ok && src_ptr != NULL && (CELL_MATERIAL(src_ptr[qx]) == CELL_MATERIAL(row[cx]));
    const unsigned src_count = qx_ok ? src_arr[qx] : 0u;

    const unsigned count =
        here_liquid ? local_depth_liquid_count(s, same_material, !cross_row || walk->chain_ok, src_count, cx, cy) : 0u;
    s->local_depth_cur_row[cx] = (uint8_t)count;
    return count;
}

/* The `depth` material_colours() takes: a root's neighbour count, a leaf
 * wave for canopy cells, else the liquid depth `count` projects to. */
static inline __attribute__((always_inline)) unsigned
cell_shading_depth(const sand_paint_row_state_t* s, const sand_paint_frame_t* pf, const uint8_t* above,
                   const uint8_t* row, const uint8_t* below, int cx, int cy, unsigned hash, bool leaf_shading,
                   unsigned count, int grid_w) {
    const unsigned depth_raw = (count * s->local_depth_scale_q8) >> 8;
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
note_row_flag(sand_paint_row_state_t* s, int cy, int cx, unsigned flag) {
    s->row_flags[cy] |= flag;
    note_row_flag_x(s, cy, cx);
}

static inline __attribute__((always_inline)) void
note_cell_row_flags(sand_paint_row_state_t* s, int cy, int cx, uint8_t cell, bool here_liquid, bool leaf_shading) {
    const unsigned cullet_first = MAT_SAND * MATERIAL_VARIANTS + SAND_CULLET_BASE;

    if (here_liquid) {
        note_row_flag(s, cy, cx, SAND_PAINT_ROW_FLAG_LIQUID);
    }
    if (leaf_shading) {
        note_row_flag(s, cy, cx, SAND_PAINT_ROW_FLAG_WOOD_LEAF);
    }
    if ((unsigned)(cell - cullet_first) < SAND_CULLET_SHADES) {
        note_row_flag(s, cy, cx, SAND_PAINT_ROW_FLAG_CULLET);
    }
    if (CELL_MATERIAL(cell) == MAT_GLASS) {
        note_row_flag(s, cy, cx, SAND_PAINT_ROW_FLAG_GLASS);
    }
}

/* MATERIAL_HATCHED's diagonal cannot survive one index per cell, but
 * whether THIS cell falls on the band still can: the same shine line,
 * sampled once at the cell's own centre instead of per pixel. A cell the
 * line crosses takes col[2]'s own index (already in the study's sweep -
 * see material_palette256_index()'s own comment) instead of col[0]'s. */
static inline __attribute__((always_inline)) void
paint_indexed_cell(sand_paint_row_state_t* s, const sand_paint_frame_t* pf, uint8_t* index_row, int cx, int cy, int n,
                   material_pattern_t pat, const gfx_color_t col[3], bool force_full) {
    gfx_color_t shade = col[0];
    if (pat == MATERIAL_HATCHED) {
        const int shine_q8 = (cx * n + n / 2) * pf->shine_ux_q8 + (cy * n + n / 2) * pf->shine_uy_q8;
        const int along = ((shine_q8 >> 8) + pf->shine_offset) & (pf->shine_period - 1);
        shade = (along < n) ? col[2] : col[0];
    }

    /* Compares against what this cell already holds - the index image is
     * never cleared between frames (only on a fresh indexed entry, see
     * apply_gfx_enter_indexed()), so it IS last frame's sent value, at no
     * extra storage. */
    const uint8_t new_idx = (uint8_t)material_palette256_index(shade);
    const uint8_t old_idx = index_row[cx];
    if (sand_indexed_cell_needs_repaint(pf, force_full, old_idx, new_idx, cx, cy)) {
        index_row[cx] = new_idx;
        note_row_change_x(s, cy, cx);
    }
}

static inline __attribute__((always_inline)) void
fill_cell_solid(gfx_color_t* p, int n, gfx_color_t c) {
    for (int dy = 0; dy < n; dy++) {
        for (int dx = 0; dx < n; dx++) {
            p[dy * GFX_WIDTH + dx] = c;
        }
    }
}

static inline __attribute__((always_inline)) void
fill_cell_hatched(const sand_paint_frame_t* pf, gfx_color_t* p, int cx, int cy, int n, const gfx_color_t col[3]) {
    const int shine_base_q8 = (cx * n) * pf->shine_ux_q8 + (cy * n) * pf->shine_uy_q8;

    for (int dy = 0; dy < n; dy++) {
        for (int dx = 0; dx < n; dx++) {

            const int shine_q8 = shine_base_q8 + dx * pf->shine_ux_q8 + dy * pf->shine_uy_q8;
            const int along = ((shine_q8 >> 8) + pf->shine_offset) & (pf->shine_period - 1);

            p[dy * GFX_WIDTH + dx] = (along < n) ? col[2] : col[0];
        }
    }
}

/* local_depth_prev_row[] checks if `toward_surface` points at it, hoisted
 * out of the cx loop for an 841-to-83 improvement. Only the
 * hold-then-commit debounce reads it, as same-material climbs trust a
 * stale count, and BOUNDARY's carry is nonsense for other rows. */
static inline __attribute__((always_inline)) local_depth_walk_t
local_depth_walk_begin(const sand_paint_row_state_t* s, int cy, const uint8_t* above, const uint8_t* below,
                       int grid_h) {
    const int local_depth_vdir = s->local_depth_v_reverse ? -1 : 1;
    return (local_depth_walk_t){
        .toward_surface = s->local_depth_v_reverse ? below : above,
        .chain_ok = (s->local_depth_prev_cy == cy - local_depth_vdir),
        .hdir = s->local_depth_h_reverse ? -1 : 1,
        .row_step = local_depth_row_step_at(s, cy, local_depth_vdir, grid_h),
        .herr = 0,
        .ysign = s->local_depth_v_reverse ? 1 : -1,
    };
}

static inline __attribute__((always_inline)) unsigned
cell_grain_hash(uint8_t cell, int cx, int cy) {
    return CELL_MATERIAL(cell) == MAT_WATER ? material_grain_hash(cx >> FOAM_BLOB_SHIFT, cy >> FOAM_BLOB_SHIFT)
                                            : material_grain_hash(cx, cy);
}

/* Every leaf cell rides the same wave unconditionally - no adjacency
 * check needed, unlike wood, since being leaf already means being part
 * of the canopy. */
static inline __attribute__((always_inline)) bool
cell_leaf_shaded(const sand_paint_frame_t* pf, const uint8_t* above, const uint8_t* row, const uint8_t* below, int cx,
                 unsigned hash, int grid_w) {
    const bool wood_near_leaf =
        row[cx] == CELL_MAKE(MAT_WOOD, 0)
        && material_wood_near_leaf(above, row, below, cx, grid_w, pf->wood_leaf_top5, hash, WOOD_LEAF_SLOTS_CHECKED);
    return wood_near_leaf || row[cx] == MATX(MATX_LEAF);
}

/* `out` is the RGB565 row the cell's n x n block starts in, unused when
 * `index_row` is non-NULL. */
static inline __attribute__((always_inline)) void
paint_cell_output(sand_paint_row_state_t* s, const sand_paint_frame_t* pf, gfx_color_t* out, uint8_t* index_row, int cx,
                  int cy, int n, material_pattern_t pat, const gfx_color_t col[3], bool force_full) {
    if (index_row != NULL) {
        paint_indexed_cell(s, pf, index_row, cx, cy, n, pat, col, force_full);
    } else if (pat != MATERIAL_HATCHED) {
        fill_cell_solid(out + cx * n, n, col[0]);
    } else {
        fill_cell_hatched(pf, out + cx * n, cx, cy, n, col);
    }
}

/* `index_row` NULL means the RGB565 path (`fb`/`pal`/`n`); non-NULL is
 * GFX_PIXFMT_INDEXED8's own grid row, writing one
 * material_palette256_index() byte per in-span cell instead. One function,
 * not two, keeps local depth, mask and the shine line - see
 * Shading-and-Colour.md - shared rather than re-derived. A cell the
 * diagonal shine crosses (sampled at the cell's own centre, not per pixel)
 * takes col[2]'s own index instead of col[0]'s. `force_full`: see
 * mark_sand_fully_dirty()'s own comment. */
static inline void
paint_row_n(sand_paint_row_state_t* s, const sand_paint_frame_t* pf, gfx_color_t* fb, const gfx_color_t* pal,
            uint8_t* index_row, int cy, const uint8_t* row, int n, int grid_w, int grid_h, int wx0, int wx1,
            bool force_full) {
    (void)pal;
    gfx_color_t* out = index_row == NULL ? fb + (cy * n) * GFX_WIDTH : NULL;
    s->row_flags[cy] = 0;
    s->row_flag_x0[cy] = (uint16_t)grid_w;
    s->row_flag_x1[cy] = 0;
    s->row_changed_x0[cy] = (uint16_t)grid_w;
    s->row_changed_x1[cy] = 0;

    const uint8_t* above = (cy > 0) ? row - grid_w : NULL;
    const uint8_t* below = (cy < grid_h - 1) ? row + grid_w : NULL;

    local_depth_walk_t walk = local_depth_walk_begin(s, cy, above, below, grid_h);

    const int cx_first = s->local_depth_h_reverse ? grid_w - 1 : 0;
    const int cx_step = s->local_depth_h_reverse ? -1 : 1;

    for (int cx_i = 0; cx_i < grid_w; cx_i++) {
        const int cx = cx_first + cx_i * cx_step;

        const unsigned mask = cell_edge_mask(above, row, below, cx, grid_w);

        const unsigned hash = cell_grain_hash(row[cx], cx, cy);

        const bool here_liquid = material_of(row[cx])->kind == KIND_LIQUID;
        const unsigned count = local_depth_count_at(s, &walk, row, cx, cy, here_liquid, grid_w);

        const bool leaf_shading = cell_leaf_shaded(pf, above, row, below, cx, hash, grid_w);

        const unsigned depth = cell_shading_depth(s, pf, above, row, below, cx, cy, hash, leaf_shading, count, grid_w);

        note_cell_row_flags(s, cy, cx, row[cx], here_liquid, leaf_shading);

        gfx_color_t col[3];
        const material_pattern_t pat = material_colours(row[cx], hash, mask, depth, col);

        if (pat == MATERIAL_HATCHED) {
            note_row_flag(s, cy, cx, SAND_PAINT_ROW_FLAG_SHINE);
        }

        /* State above (local depth, row_flags, hash) runs the full row
         * regardless - only pixels reaching the framebuffer are bounded to
         * [wx0,wx1). A cx outside it has provably unchanged output: every
         * mark site already widens for the reach its own output depends on. */
        const bool in_span = cx >= wx0 && cx < wx1;
        if (in_span) {
            paint_cell_output(s, pf, out, index_row, cx, cy, n, pat, col, force_full);
        }
    }

    uint8_t* local_depth_tmp = s->local_depth_cur_row;
    s->local_depth_cur_row = s->local_depth_prev_row;
    s->local_depth_prev_row = local_depth_tmp;

    s->local_depth_prev_cy = cy;
}
