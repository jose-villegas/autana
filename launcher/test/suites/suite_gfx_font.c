/*
 * Portable suite: gfx_font - the pure metrics half of a font descriptor,
 * plus gfx_font_row_run_rect()'s screen-space geometry.
 *
 * gfx_font.h splits a font into pure metrics (gfx_font_advance(),
 * gfx_font_text_width(), gfx_font_height() - `static inline` in the header,
 * same reason icon_walk_blocks() is in gfx/icon.h: it links on a host with no
 * gfx.h, no BSP, no drivers) and drawing (gfx_text_font() in gfx.c, which
 * calls gfx_fill_rect() and so cannot). This suite exercises the metrics and
 * the geometry, the same split suite_icons.c makes for gfx/icons_system.h -
 * gfx_font_row_run_rect() computes where a rect goes, gfx.c only calls it.
 *
 * gfx.h is deliberately NOT included here - it pulls in bsp/esp-bsp.h, which
 * does not compile on a host. That means GFX_CHAR_W, GFX_CHAR_H and
 * GFX_GLYPH_SCALE (gfx.h) are not reachable from this file, so the few
 * assertions that need "the size the UI is laid out around" mirror
 * GFX_GLYPH_SCALE as a local constant instead of including it - see
 * MIRRORED_GLYPH_SCALE below. Everything else is derived from
 * gfx_font_8x8's own fields (cell_w, cell_h) rather than a second hardcoded
 * 8, so a change to the bitmap's cell size cannot silently drift out of
 * step with what this suite expects.
 */

#include <string.h>

#include "suites.h"
#include "unity.h"

#include "gfx/gfx_font.h"
#include "gfx/gfx_target.h"

/* Mirrors gfx.h's GFX_GLYPH_SCALE (8x8 glyphs drawn at 2x - see gfx.h's own
 * comment on GFX_CHAR_W/GFX_CHAR_H). Kept in one place, right where it is
 * used, rather than repeated as a magic 2 at every call site below. */
#define MIRRORED_GLYPH_SCALE 2

/* gfx_font_8x8 - the real, shipped font */

static void
test_default_font_width_matches_char_w_per_character(void) {
    /* GFX_CHAR_W is 8 * GFX_GLYPH_SCALE (gfx.h) - i.e. cell_w * scale here. */
    const int char_w = gfx_font_8x8.cell_w * MIRRORED_GLYPH_SCALE;

    TEST_ASSERT_EQUAL_INT(char_w, gfx_font_text_width(&gfx_font_8x8, "A", 1, MIRRORED_GLYPH_SCALE));
    TEST_ASSERT_EQUAL_INT(3 * char_w, gfx_font_text_width(&gfx_font_8x8, "ABC", 3, MIRRORED_GLYPH_SCALE));
}

static void
test_width_scales_linearly_with_scale(void) {
    const int one_char_at_1 = gfx_font_text_width(&gfx_font_8x8, "A", 1, 1);

    for (int scale = 1; scale <= 5; scale++) {
        TEST_ASSERT_EQUAL_INT_MESSAGE(one_char_at_1 * scale, gfx_font_text_width(&gfx_font_8x8, "A", 1, scale),
                                      "width did not scale linearly with `scale`");
    }
}

static void
test_width_scales_linearly_with_length(void) {
    const int one_char = gfx_font_text_width(&gfx_font_8x8, "A", 1, MIRRORED_GLYPH_SCALE);

    for (int len = 0; len <= 10; len++) {
        TEST_ASSERT_EQUAL_INT_MESSAGE(one_char * len,
                                      gfx_font_text_width(&gfx_font_8x8, "AAAAAAAAAA", len, MIRRORED_GLYPH_SCALE),
                                      "width did not scale linearly with string length");
    }
}

/* The monospace shortcut (gfx_font_text_width's `advance == NULL` branch)
 * never inspects the string's content when `len >= 0`. A `len` longer
 * than what a literal actually holds must therefore still be safe (and
 * still just len * cell_w * scale), not a
 * newly-introduced out-of-bounds read. */
static void
test_monospace_width_does_not_need_len_to_fit_the_string(void) {
    const int expect = 20 * gfx_font_8x8.cell_w * MIRRORED_GLYPH_SCALE;
    TEST_ASSERT_EQUAL_INT(expect, gfx_font_text_width(&gfx_font_8x8, "HI", 20, MIRRORED_GLYPH_SCALE));
}

static void
test_len_negative_matches_nul_terminated_length(void) {
    const char* s = "hello, world";
    TEST_ASSERT_EQUAL_INT(gfx_font_text_width(&gfx_font_8x8, s, (int)strlen(s), 1),
                          gfx_font_text_width(&gfx_font_8x8, s, -1, 1));
}

static void
test_zero_length_string_is_zero_wide(void) {
    TEST_ASSERT_EQUAL_INT(0, gfx_font_text_width(&gfx_font_8x8, "", -1, MIRRORED_GLYPH_SCALE));
    TEST_ASSERT_EQUAL_INT(0, gfx_font_text_width(&gfx_font_8x8, "ignored", 0, MIRRORED_GLYPH_SCALE));
}

static void
test_height_matches_char_h_at_default_scale(void) {
    /* GFX_CHAR_H is 8 * GFX_GLYPH_SCALE (gfx.h) - i.e. cell_h * scale here. */
    const int char_h = gfx_font_8x8.cell_h * MIRRORED_GLYPH_SCALE;
    TEST_ASSERT_EQUAL_INT(char_h, gfx_font_height(&gfx_font_8x8, MIRRORED_GLYPH_SCALE));
}

/* Monospace: every glyph advances by cell_w * scale, whether or not the
 * codepoint is one the font actually covers - the same thing the old
 * gfx_text_turned() did, advancing by a fixed cell every character even
 * past one it declined to draw (see gfx.c's gfx_text_font()). */
static void
test_monospace_advance_is_cell_w_times_scale_for_every_glyph(void) {
    const unsigned char in_range[] = {0, 'A', 'z', 127};
    const unsigned char out_of_range[] = {128, 200, 255};

    for (size_t i = 0; i < sizeof(in_range); i++) {
        TEST_ASSERT_EQUAL_INT(gfx_font_8x8.cell_w * MIRRORED_GLYPH_SCALE,
                              gfx_font_advance(&gfx_font_8x8, in_range[i], MIRRORED_GLYPH_SCALE));
    }
    for (size_t i = 0; i < sizeof(out_of_range); i++) {
        TEST_ASSERT_EQUAL_INT_MESSAGE(gfx_font_8x8.cell_w * MIRRORED_GLYPH_SCALE,
                                      gfx_font_advance(&gfx_font_8x8, out_of_range[i], MIRRORED_GLYPH_SCALE),
                                      "an out-of-range codepoint must still advance by a full cell, "
                                      "matching the old gfx_text_turned()'s unconditional advance");
    }
}

/*
 * A synthetic proportional descriptor - the case with no real font behind
 * it yet, and so the easiest to get silently wrong (a monospace-only test
 * would still pass even if `advance` were never actually consulted).
 */

/* Covers 'A'..'D' (4 glyphs) with deliberately distinct per-glyph advances,
 * so a bug that returns the wrong glyph's advance, or falls through to the
 * monospace default, shows up as a wrong number rather than an accident of
 * every glyph having the same width. The atlas is never read by these
 * tests (nothing here draws), so it is left zeroed. */
static const uint8_t synth_atlas[4 * 7] = {0};
static const uint8_t synth_advance[4] = {3, 4, 5, 6};
static const gfx_font_t synth_font = {
    .atlas = synth_atlas,
    .bpp = 1,
    .cell_w = 5,
    .cell_h = 7,
    .first = (uint8_t)'A',
    .count = 4,
    .advance = synth_advance,
};

static void
test_proportional_advance_is_per_glyph(void) {
    TEST_ASSERT_EQUAL_INT(3, gfx_font_advance(&synth_font, 'A', 1));
    TEST_ASSERT_EQUAL_INT(4, gfx_font_advance(&synth_font, 'B', 1));
    TEST_ASSERT_EQUAL_INT(5, gfx_font_advance(&synth_font, 'C', 1));
    TEST_ASSERT_EQUAL_INT(6, gfx_font_advance(&synth_font, 'D', 1));

    TEST_ASSERT_EQUAL_INT(6, gfx_font_advance(&synth_font, 'A', 2));
    TEST_ASSERT_EQUAL_INT(12, gfx_font_advance(&synth_font, 'D', 2));
}

static void
test_proportional_advance_falls_back_outside_its_range(void) {
    /* 'Z' and the space before 'A' are both outside [first, first+count) -
     * neither has an entry in synth_advance, so both must fall back to the
     * monospace cell_w * scale rather than reading synth_advance
     * out-of-bounds. */
    TEST_ASSERT_EQUAL_INT(synth_font.cell_w, gfx_font_advance(&synth_font, 'Z', 1));
    TEST_ASSERT_EQUAL_INT(synth_font.cell_w, gfx_font_advance(&synth_font, ' ', 1));
    TEST_ASSERT_EQUAL_INT(synth_font.cell_w * 3, gfx_font_advance(&synth_font, 'Z', 3));
}

static void
test_proportional_text_width_sums_per_glyph_advances(void) {
    /* "ABCD" -> 3 + 4 + 5 + 6 = 18 at scale 1, 36 at scale 2. Unlike the
     * monospace shortcut, this path genuinely reads each of the `len`
     * characters, so - deliberately, unlike the monospace test above - `len`
     * here never exceeds what the literal actually holds. */
    TEST_ASSERT_EQUAL_INT(18, gfx_font_text_width(&synth_font, "ABCD", 4, 1));
    TEST_ASSERT_EQUAL_INT(36, gfx_font_text_width(&synth_font, "ABCD", 4, 2));

    /* A prefix stops summing where `len` says to, not at the string's own
     * end - "AB" worth of width out of the longer "ABCD" literal. */
    TEST_ASSERT_EQUAL_INT(3 + 4, gfx_font_text_width(&synth_font, "ABCD", 2, 1));
}

static void
test_proportional_text_width_len_negative_is_nul_terminated(void) {
    TEST_ASSERT_EQUAL_INT(3 + 4, gfx_font_text_width(&synth_font, "AB", -1, 1));
}

static void
test_proportional_height_is_cell_h_times_scale(void) {
    TEST_ASSERT_EQUAL_INT(7, gfx_font_height(&synth_font, 1));
    TEST_ASSERT_EQUAL_INT(21, gfx_font_height(&synth_font, 3));
}

/*
 * gfx_font_row_run_rect() - proof that batching a run of set bits into one
 * rect covers exactly the same pixels as gfx.c's draw_rotated_font_pixel()
 * would have, one bit at a time. gfx.c cannot link on a host (it calls
 * gfx_fill_rect()), so draw_rotated_font_pixel()'s px/py switch is mirrored
 * here rather than driven directly - see this suite's own file comment.
 */

static void
reference_unit_rect(const gfx_font_t* f, int x, int y, int row, int col, int scale, int turn, int* out_x, int* out_y) {
    int px, py;
    switch (turn) {
        case 1:
            px = f->cell_h - 1 - row;
            py = col;
            break;
        case 2:
            px = f->cell_w - 1 - col;
            py = f->cell_h - 1 - row;
            break;
        case 3:
            px = row;
            py = f->cell_w - 1 - col;
            break;
        default:
            px = col;
            py = row;
            break;
    }
    *out_x = x + px * scale;
    *out_y = y + py * scale;
}

/* Every unit cell in [col0, col1] at `turn`, unioned, must equal the one
 * rect gfx_font_row_run_rect() returns - exactly, not just in area, or a
 * gap or an off-by-one overlap could slip through unnoticed. */
static void
assert_run_rect_matches_reference(const gfx_font_t* f, int x, int y, int row, int col0, int col1, int scale, int turn) {
    int union_x0 = 0, union_y0 = 0, union_x1 = 0, union_y1 = 0;
    for (int col = col0; col <= col1; col++) {
        int ux, uy;
        reference_unit_rect(f, x, y, row, col, scale, turn, &ux, &uy);
        const int ux1 = ux + scale, uy1 = uy + scale;
        if (col == col0) {
            union_x0 = ux;
            union_y0 = uy;
            union_x1 = ux1;
            union_y1 = uy1;
        } else {
            if (ux < union_x0) {
                union_x0 = ux;
            }
            if (uy < union_y0) {
                union_y0 = uy;
            }
            if (ux1 > union_x1) {
                union_x1 = ux1;
            }
            if (uy1 > union_y1) {
                union_y1 = uy1;
            }
        }
    }

    int rx, ry, rw, rh;
    gfx_font_row_run_rect(f, x, y, row, col0, col1, scale, turn, &rx, &ry, &rw, &rh);

    TEST_ASSERT_EQUAL_INT_MESSAGE(union_x0, rx, "run rect's left edge");
    TEST_ASSERT_EQUAL_INT_MESSAGE(union_y0, ry, "run rect's top edge");
    TEST_ASSERT_EQUAL_INT_MESSAGE(union_x1 - union_x0, rw, "run rect's width");
    TEST_ASSERT_EQUAL_INT_MESSAGE(union_y1 - union_y0, rh, "run rect's height");
}

static void
test_row_run_rect_matches_per_bit_placement_at_every_turn(void) {
    /* A single bit, a run in the middle, and a run touching each edge of
     * the cell - across all four turns, gfx_font_8x8's own 8x8 shape. */
    const int col0s[] = {3, 0, 2, 0};
    const int col1s[] = {3, 2, 7, 7};

    for (int turn = 0; turn < 4; turn++) {
        for (size_t i = 0; i < sizeof(col0s) / sizeof(col0s[0]); i++) {
            for (int row = 0; row < gfx_font_8x8.cell_h; row++) {
                assert_run_rect_matches_reference(&gfx_font_8x8, 10, 20, row, col0s[i], col1s[i], 2, turn);
            }
        }
    }
}

static void
test_row_run_rect_matches_per_bit_placement_at_scale_one_and_at_origin(void) {
    /* scale 1 and (x, y) == (0, 0) are the values most likely to hide an
     * off-by-one that a larger scale or offset would smear across many
     * pixels instead. */
    for (int turn = 0; turn < 4; turn++) {
        assert_run_rect_matches_reference(&gfx_font_8x8, 0, 0, 5, 0, 7, 1, turn);
    }
}

/*
 * gfx_text_font() (gfx.c) skips a whole character when its own row extent
 * misses the current gfx_target_t entirely - one command replayed into
 * several bands (ui_replay_band()) otherwise re-walks every character per
 * band. Safe only if every rect gfx_font_row_run_rect() can ever produce
 * for that character stays inside the same row extent, so skipping never
 * discards a rect that would have painted anything - proven here for every
 * (row, col) a real glyph can pass, at every turn, without gfx.c.
 */
static void
test_row_run_rect_never_leaves_the_characters_own_row_extent(void) {
    const int x = 7, y = 30, scale = 3;

    for (int turn = 0; turn < 4; turn++) {
        const int char_h = (turn & 1) ? gfx_font_8x8.cell_w * scale : gfx_font_8x8.cell_h * scale;

        for (int row = 0; row < gfx_font_8x8.cell_h; row++) {
            for (int col0 = 0; col0 < gfx_font_8x8.cell_w; col0++) {
                for (int col1 = col0; col1 < gfx_font_8x8.cell_w; col1++) {
                    int rx, ry, rw, rh;
                    gfx_font_row_run_rect(&gfx_font_8x8, x, y, row, col0, col1, scale, turn, &rx, &ry, &rw, &rh);

                    TEST_ASSERT_TRUE_MESSAGE(ry >= y, "a run rect must not start above the character's own extent");
                    TEST_ASSERT_TRUE_MESSAGE(ry + rh <= y + char_h,
                                             "a run rect must not reach below the character's own extent");
                }
            }
        }
    }
}

/*
 * gfx_font_row_run_rect_dilated() - proof that one dilated-halo pass
 * paints the same pixels as UI_TEXT_OUTLINED's 8 unit-offset copies of
 * gfx_text_font(), ink drawn last either way. Rasterizes both forms into
 * small pixel grids via gfx_target_fill_rect() (the same body gfx.c's
 * gfx_fill_rect() calls) and compares them exactly, at every turn and
 * with a band edge cutting through the glyph.
 */

#define SIM_DIM  40
#define SIM_INK  ((gfx_color_t)1)
#define SIM_HALO ((gfx_color_t)2)

static gfx_color_t sim_old[SIM_DIM * SIM_DIM];
static gfx_color_t sim_new[SIM_DIM * SIM_DIM];

typedef void (*run_rect_fn_t)(const gfx_font_t*, int, int, int, int, int, int, int, int*, int*, int*, int*);

static void
sim_walk_runs(gfx_target_t target, const gfx_font_t* f, int x, int y, unsigned char ch, int scale, int turn,
              run_rect_fn_t rect_fn, gfx_color_t color) {
    const uint8_t* glyph = f->atlas + (size_t)(ch - f->first) * f->cell_h;
    for (int row = 0; row < f->cell_h; row++) {
        const uint8_t bits = glyph[row];
        if (bits == 0) {
            continue;
        }
        int col = 0;
        while (col < f->cell_w) {
            if (!(bits & (1 << col))) {
                col++;
                continue;
            }
            int end = col;
            while (end + 1 < f->cell_w && (bits & (1 << (end + 1)))) {
                end++;
            }
            int rx, ry, rw, rh, ox0, oy0, ox1, oy1;
            rect_fn(f, x, y, row, col, end, scale, turn, &rx, &ry, &rw, &rh);
            gfx_target_fill_rect(target, 0, 0, SIM_DIM, SIM_DIM, rx, ry, rw, rh, color, &ox0, &oy0, &ox1, &oy1);
            col = end + 1;
        }
    }
}

/* draw_command()'s own 8-offset loop, at the row-run level: SIM_HALO at
 * each of the 8 unit screen-space offsets, SIM_INK last, unshifted. */
static void
sim_draw_old(gfx_target_t target, const gfx_font_t* f, int x, int y, unsigned char ch, int scale, int turn) {
    static const int offsets[8][2] = {
        {-1, -1}, {0, -1}, {1, -1}, {-1, 0}, {1, 0}, {-1, 1}, {0, 1}, {1, 1},
    };
    for (int i = 0; i < 8; i++) {
        sim_walk_runs(target, f, x + offsets[i][0], y + offsets[i][1], ch, scale, turn, gfx_font_row_run_rect,
                      SIM_HALO);
    }
    sim_walk_runs(target, f, x, y, ch, scale, turn, gfx_font_row_run_rect, SIM_INK);
}

/* gfx_text_font_halo()/gfx_text_font() at the row-run level: one dilated
 * SIM_HALO pass, SIM_INK last, unshifted - draw_command()'s fast path. */
static void
sim_draw_new(gfx_target_t target, const gfx_font_t* f, int x, int y, unsigned char ch, int scale, int turn) {
    sim_walk_runs(target, f, x, y, ch, scale, turn, gfx_font_row_run_rect_dilated, SIM_HALO);
    sim_walk_runs(target, f, x, y, ch, scale, turn, gfx_font_row_run_rect, SIM_INK);
}

/* `band` gives the row range to draw into - its own buf is ignored and
 * replaced with sim_old/sim_new so both forms land in a real buffer. */
static void
assert_old_and_new_match(gfx_target_t band, int x, int y, unsigned char ch, int scale, int turn) {
    memset(sim_old, 0, sizeof sim_old);
    memset(sim_new, 0, sizeof sim_new);

    gfx_target_t old_target = band, new_target = band;
    old_target.buf = sim_old;
    new_target.buf = sim_new;

    sim_draw_old(old_target, &gfx_font_8x8, x, y, ch, scale, turn);
    sim_draw_new(new_target, &gfx_font_8x8, x, y, ch, scale, turn);

    TEST_ASSERT_EQUAL_UINT16_ARRAY_MESSAGE(sim_old, sim_new, SIM_DIM * SIM_DIM,
                                           "one dilated halo pass must paint exactly what 8 offset copies do");
}

static void
test_dilated_halo_matches_eight_offset_copies_at_every_turn(void) {
    const gfx_target_t full = {NULL, 0, SIM_DIM, SIM_DIM};

    for (int turn = 0; turn < 4; turn++) {
        for (unsigned char ch = 'A'; ch <= 'Z'; ch++) {
            assert_old_and_new_match(full, 15, 15, ch, 2, turn);
        }
    }
}

/* A band edge cutting straight through the glyph's own rows - the case
 * ui_replay_band() creates for real: the target's own row range narrower
 * than SIM_DIM, clipping both forms the same way gfx_target_clip_y()
 * always does. */
static void
test_dilated_halo_matches_eight_offset_copies_at_a_band_edge(void) {
    const gfx_target_t band = {NULL, 10, 8, SIM_DIM}; /* rows [10, 18) only */

    for (int turn = 0; turn < 4; turn++) {
        assert_old_and_new_match(band, 15, 15, 'A', 2, turn);
    }
}

/*
 * gfx_font_glyph_run_boxes() - the merge itself, against synthetic glyphs
 * whose exact box output is known by construction, plus proof that
 * merging changes no pixel real letters draw, at every turn and a band
 * edge.
 */

/* Rows 0-2 share one run (cols 1-2), rows 3-7 share another (cols 3-4) -
 * two boxes, not eight one-row runs. */
static const uint8_t merge_synth_atlas[8] = {0x06, 0x06, 0x06, 0x18, 0x18, 0x18, 0x18, 0x18};
static const gfx_font_t merge_synth_font = {
    .atlas = merge_synth_atlas,
    .bpp = 1,
    .cell_w = 8,
    .cell_h = 8,
    .first = (uint8_t)'A',
    .count = 1,
    .advance = NULL,
};

static void
test_glyph_run_boxes_merges_consecutive_identical_rows(void) {
    gfx_font_run_box_t boxes[GFX_FONT_RUN_BOXES_MAX];
    const int n = gfx_font_glyph_run_boxes(&merge_synth_font, 'A', boxes, GFX_FONT_RUN_BOXES_MAX);

    TEST_ASSERT_EQUAL_INT(2, n);
    TEST_ASSERT_EQUAL_INT(0, boxes[0].row0);
    TEST_ASSERT_EQUAL_INT(3, boxes[0].row1);
    TEST_ASSERT_EQUAL_INT(1, boxes[0].col0);
    TEST_ASSERT_EQUAL_INT(2, boxes[0].col1);
    TEST_ASSERT_EQUAL_INT(3, boxes[1].row0);
    TEST_ASSERT_EQUAL_INT(8, boxes[1].row1);
    TEST_ASSERT_EQUAL_INT(3, boxes[1].col0);
    TEST_ASSERT_EQUAL_INT(4, boxes[1].col1);
}

/* The same [1, 2] run at rows 0 and 2, but not row 1 - two single-row
 * boxes, since a gap must not bridge a merge. */
static const uint8_t merge_gap_atlas[8] = {0x06, 0x00, 0x06, 0x00, 0x00, 0x00, 0x00, 0x00};
static const gfx_font_t merge_gap_font = {
    .atlas = merge_gap_atlas,
    .bpp = 1,
    .cell_w = 8,
    .cell_h = 8,
    .first = (uint8_t)'A',
    .count = 1,
    .advance = NULL,
};

static void
test_glyph_run_boxes_does_not_merge_across_a_gap_row(void) {
    gfx_font_run_box_t boxes[GFX_FONT_RUN_BOXES_MAX];
    const int n = gfx_font_glyph_run_boxes(&merge_gap_font, 'A', boxes, GFX_FONT_RUN_BOXES_MAX);

    TEST_ASSERT_EQUAL_INT(2, n);
    TEST_ASSERT_EQUAL_INT(0, boxes[0].row0);
    TEST_ASSERT_EQUAL_INT(1, boxes[0].row1);
    TEST_ASSERT_EQUAL_INT(2, boxes[1].row0);
    TEST_ASSERT_EQUAL_INT(3, boxes[1].row1);
}

static void
test_glyph_run_boxes_empty_glyph_yields_none(void) {
    static const uint8_t blank[8] = {0};
    static const gfx_font_t blank_font = {
        .atlas = blank,
        .bpp = 1,
        .cell_w = 8,
        .cell_h = 8,
        .first = (uint8_t)'A',
        .count = 1,
        .advance = NULL,
    };
    gfx_font_run_box_t boxes[GFX_FONT_RUN_BOXES_MAX];
    TEST_ASSERT_EQUAL_INT(0, gfx_font_glyph_run_boxes(&blank_font, 'A', boxes, GFX_FONT_RUN_BOXES_MAX));
}

static void
test_glyph_run_boxes_out_of_range_char_yields_none(void) {
    gfx_font_run_box_t boxes[GFX_FONT_RUN_BOXES_MAX];
    TEST_ASSERT_EQUAL_INT(0, gfx_font_glyph_run_boxes(&merge_synth_font, 'Z', boxes, GFX_FONT_RUN_BOXES_MAX));
}

typedef void (*run_box_fn_t)(const gfx_font_t*, int, int, int, int, int, int, int, int, int*, int*, int*, int*);

static void
sim_walk_boxes(gfx_target_t target, const gfx_font_t* f, int x, int y, unsigned char ch, int scale, int turn,
               run_box_fn_t box_fn, gfx_color_t color) {
    gfx_font_run_box_t boxes[GFX_FONT_RUN_BOXES_MAX];
    const int n = gfx_font_glyph_run_boxes(f, ch, boxes, GFX_FONT_RUN_BOXES_MAX);
    for (int i = 0; i < n; i++) {
        int rx, ry, rw, rh, ox0, oy0, ox1, oy1;
        box_fn(f, x, y, boxes[i].row0, boxes[i].row1, boxes[i].col0, boxes[i].col1, scale, turn, &rx, &ry, &rw, &rh);
        gfx_target_fill_rect(target, 0, 0, SIM_DIM, SIM_DIM, rx, ry, rw, rh, color, &ox0, &oy0, &ox1, &oy1);
    }
}

/* draw_glyph_font()/draw_glyph_font_halo()'s own merged-box shape: one
 * dilated-box halo pass, one plain-box ink pass. */
static void
sim_draw_merged(gfx_target_t target, const gfx_font_t* f, int x, int y, unsigned char ch, int scale, int turn) {
    sim_walk_boxes(target, f, x, y, ch, scale, turn, gfx_font_run_box_rect_dilated, SIM_HALO);
    sim_walk_boxes(target, f, x, y, ch, scale, turn, gfx_font_run_box_rect, SIM_INK);
}

static gfx_color_t sim_merged[SIM_DIM * SIM_DIM];

static void
assert_merged_matches_unmerged(gfx_target_t band, int x, int y, unsigned char ch, int scale, int turn) {
    memset(sim_new, 0, sizeof sim_new);
    memset(sim_merged, 0, sizeof sim_merged);

    gfx_target_t unmerged_target = band, merged_target = band;
    unmerged_target.buf = sim_new;
    merged_target.buf = sim_merged;

    sim_draw_new(unmerged_target, &gfx_font_8x8, x, y, ch, scale, turn);
    sim_draw_merged(merged_target, &gfx_font_8x8, x, y, ch, scale, turn);

    TEST_ASSERT_EQUAL_UINT16_ARRAY_MESSAGE(sim_new, sim_merged, SIM_DIM * SIM_DIM,
                                           "merging identical consecutive-row runs must not change any pixel");
}

static void
test_merged_boxes_match_unmerged_runs_at_every_turn(void) {
    const gfx_target_t full = {NULL, 0, SIM_DIM, SIM_DIM};

    for (int turn = 0; turn < 4; turn++) {
        for (unsigned char ch = 'A'; ch <= 'Z'; ch++) {
            assert_merged_matches_unmerged(full, 15, 15, ch, 2, turn);
        }
    }
}

/* A band edge cutting through a merged, multi-row box - the case a tall
 * merged rect must still clip correctly against. */
static void
test_merged_boxes_match_unmerged_runs_at_a_band_edge(void) {
    const gfx_target_t band = {NULL, 10, 8, SIM_DIM}; /* rows [10, 18) only */

    for (int turn = 0; turn < 4; turn++) {
        assert_merged_matches_unmerged(band, 15, 15, 'A', 2, turn);
    }
}

void
run_gfx_font_suite(void) {
    RUN_TEST(test_default_font_width_matches_char_w_per_character);
    RUN_TEST(test_width_scales_linearly_with_scale);
    RUN_TEST(test_width_scales_linearly_with_length);
    RUN_TEST(test_monospace_width_does_not_need_len_to_fit_the_string);
    RUN_TEST(test_len_negative_matches_nul_terminated_length);
    RUN_TEST(test_zero_length_string_is_zero_wide);
    RUN_TEST(test_height_matches_char_h_at_default_scale);
    RUN_TEST(test_monospace_advance_is_cell_w_times_scale_for_every_glyph);
    RUN_TEST(test_proportional_advance_is_per_glyph);
    RUN_TEST(test_proportional_advance_falls_back_outside_its_range);
    RUN_TEST(test_proportional_text_width_sums_per_glyph_advances);
    RUN_TEST(test_proportional_text_width_len_negative_is_nul_terminated);
    RUN_TEST(test_proportional_height_is_cell_h_times_scale);
    RUN_TEST(test_row_run_rect_matches_per_bit_placement_at_every_turn);
    RUN_TEST(test_row_run_rect_matches_per_bit_placement_at_scale_one_and_at_origin);
    RUN_TEST(test_row_run_rect_never_leaves_the_characters_own_row_extent);
    RUN_TEST(test_dilated_halo_matches_eight_offset_copies_at_every_turn);
    RUN_TEST(test_dilated_halo_matches_eight_offset_copies_at_a_band_edge);
    RUN_TEST(test_glyph_run_boxes_merges_consecutive_identical_rows);
    RUN_TEST(test_glyph_run_boxes_does_not_merge_across_a_gap_row);
    RUN_TEST(test_glyph_run_boxes_empty_glyph_yields_none);
    RUN_TEST(test_glyph_run_boxes_out_of_range_char_yields_none);
    RUN_TEST(test_merged_boxes_match_unmerged_runs_at_every_turn);
    RUN_TEST(test_merged_boxes_match_unmerged_runs_at_a_band_edge);
}

SUITE_REGISTER(run_gfx_font_suite);
