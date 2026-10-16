#include "draw/draw.h"
#include "gfx/gfx.h"
#include "gfx/target/gfx_segment_clip.h"
#include "math/scalar/mathi.h"
#include "services/tune.h"

#include <string.h>

TUNE_OWNER(draw);

/* Tight fill marks retain each clipped box; row marks claim full width.
 * The tunable isolates band-skipping faults from caller marking faults. */
TUNE(draw, tight_fill_marks, 1, 0, 1);

/* What every rect and blit primitive marks with. A release build folds
 * tight_fill_marks to the constant 1 (services/tune.h), so the branch and
 * gfx_mark_rows_dirty() call below cost nothing there. */
static inline void
mark_fill(const gfx_surface_t* state, recti_t rect) {
    if (!state->marks) {
        return;
    }
    if (tight_fill_marks) {
        gfx_mark_drawn(rect.x, rect.y, rect.w, rect.h);
    } else {
        gfx_mark_rows_dirty(rect.y, recti_bottom(rect));
    }
}

/* Clipping */

void
draw_set_clip(int x, int y, int w, int h) {
    gfx_set_clip(recti_intersect((recti_t){x, y, w, h}, GFX_SCREEN_RECT));
}

void
draw_clear_clip(void) {
    gfx_set_clip(GFX_SCREEN_RECT);
}

/* Primitives */

/* Ignores clip rect; clears the whole target. Band mode resends every
 * band every frame, so dirty tracking applies only to the framebuffer. */
void
draw_clear(gfx_color_t color) {
    gfx_surface_t state;
    if (!gfx_draw_begin(&state)) {
        return;
    }
    const uint32_t pair = ((uint32_t)color << 16) | color;
    uint32_t* words = (uint32_t*)state.target.buf;
    const int count = (state.target.stride * state.target.height) / 2;

    for (int i = 0; i < count; i++) {
        words[i] = pair;
    }

    if (state.marks) {
        gfx_mark_all_dirty();
    }
}

void
draw_pixel(int x, int y, gfx_color_t color) {
    gfx_surface_t state;
    if (!gfx_draw_begin(&state)) {
        return;
    }
    if (!recti_contains(state.clip, x, y) || !gfx_target_row_range_overlaps(state.target, y, y + 1)) {
        return;
    }
    gfx_target_row(state.target, y)[x] = color;
    if (state.marks) {
        gfx_mark_rows_dirty(y, y + 1);
    }
}

/* One pixel of a line already clipped to the surface's clip; only the
 * target's own rows remain to check. */
static void
plot(const gfx_surface_t* state, int x, int y, gfx_color_t color, unsigned flags) {
    if (!gfx_target_row_range_overlaps(state->target, y, y + 1)) {
        return;
    }
    gfx_color_t* const dst = &gfx_target_row(state->target, y)[x];

    *dst = (flags & DRAW_LINE_ADD) ? gfx_color_add(*dst, color) : color;
}

/* Bresenham (docs/Citations.md#30), treats both axes alike, no case analysis. */
static void
walk(const gfx_surface_t* state, int x0, int y0, int x1, int y1, gfx_color_t color, unsigned flags) {
    const int dx = mathi_abs(x1 - x0);
    const int dy = -mathi_abs(y1 - y0);
    const int sx = x0 < x1 ? 1 : -1;
    const int sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;
    bool first = true;

    for (;;) {
        if (!(first && (flags & DRAW_LINE_OPEN))) {
            plot(state, x0, y0, color, flags);
        }
        first = false;

        if (x0 == x1 && y0 == y1) {
            break;
        }
        const int e2 = 2 * err;
        if (e2 >= dy) {
            err += dy;
            x0 += sx;
        }
        if (e2 <= dx) {
            err += dx;
            y0 += sy;
        }
    }
}

/* Clipped endpoints lie inside the clip, so every pixel between them does,
 * and their box is the dirty region, marked once. Overestimating costs bus
 * time; underestimating leaves stale. */
static void
line_walk(const gfx_surface_t* state, int x0, int y0, int x1, int y1, gfx_color_t color, unsigned flags) {
    if (!gfx_clip_segment(&state->clip, &x0, &y0, &x1, &y1)) {
        return;
    }

    walk(state, x0, y0, x1, y1, color, flags);

    if (state->marks) {
        gfx_mark_drawn(mathi_min(x0, x1), mathi_min(y0, y1), mathi_abs(x1 - x0) + 1, mathi_abs(y1 - y0) + 1);
    }
}

void
draw_line(int x0, int y0, int x1, int y1, gfx_color_t color) {
    gfx_surface_t state;
    if (!gfx_draw_begin(&state)) {
        return;
    }
    line_walk(&state, x0, y0, x1, y1, color, 0);
}

void
draw_line_ex(int x0, int y0, int x1, int y1, gfx_color_t color, unsigned flags) {
    gfx_surface_t state;
    if (!gfx_draw_begin(&state)) {
        return;
    }
    line_walk(&state, x0, y0, x1, y1, color, flags);
}

void
draw_rect(int x, int y, int w, int h, gfx_color_t color) {
    gfx_surface_t state;
    if (!gfx_draw_begin(&state)) {
        return;
    }

    const recti_t painted = gfx_target_fill_rect(state.target, state.clip, (recti_t){x, y, w, h}, color);

    mark_fill(&state, painted);
}

/*
 * Dithered fake transparency
 *
 * draw_rect_blend() (further down) is a REAL per-pixel blend, but pays
 * for a framebuffer read, affordable at glyph scale, not a whole frame
 * (its own comment). Dithering fakes transparency instead: ordered (Bayer)
 * dithering picks WHICH pixels to draw via a per-pixel threshold, no
 * framebuffer read, no float math.
 */

/* Dither coverage avoids framebuffer reads; blending reads each destination pixel. */
static inline __attribute__((always_inline)) void
fill_rect_alpha(const gfx_surface_t* state, int x, int y, int w, int h, gfx_color_t color, uint8_t alpha, bool blend) {
    if (alpha == 0) {
        return;
    }

    const recti_t rect = gfx_target_clip_rect(state->target, state->clip, (recti_t){x, y, w, h});

    for (int row = rect.y; row < recti_bottom(rect); row++) {
        gfx_color_t* dst = gfx_target_row(state->target, row);
        for (int i = 0; i < rect.w; i++) {
            const int col = rect.x + i;
            if (blend) {
                dst[col] = gfx_color_mix(dst[col], color, alpha);
            } else if (gfx_dither_covers(col, row, alpha)) {
                dst[col] = color;
            }
        }
    }

    mark_fill(state, rect);
}

void
draw_rect_dither(int x, int y, int w, int h, gfx_color_t color, uint8_t alpha) {
    gfx_surface_t state;
    if (!gfx_draw_begin(&state)) {
        return;
    }
    fill_rect_alpha(&state, x, y, w, h, color, alpha, false);
}

void
draw_rect_blend(int x, int y, int w, int h, gfx_color_t color, uint8_t alpha) {
    gfx_surface_t state;
    if (!gfx_draw_begin(&state)) {
        return;
    }
    fill_rect_alpha(&state, x, y, w, h, color, alpha, true);
}

/* `dp`/`sp` start at column `x0`; only the columns `p` passes are copied. */
static inline __attribute__((always_inline)) void
blit_dither_row(gfx_color_t* dp, const gfx_color_t* sp, int x0, int x1, const bool p[4]) {
    int col = x0;
    for (; col < x1 && (col & 3) != 0; col++, dp++, sp++) {
        if (p[col & 3]) {
            *dp = *sp;
        }
    }
    for (; col + 4 <= x1; col += 4, dp += 4, sp += 4) {
        for (int k = 0; k < 4; k++) {
            if (p[k]) {
                dp[k] = sp[k];
            }
        }
    }
    for (; col < x1; col++, dp++, sp++) {
        if (p[col & 3]) {
            *dp = *sp;
        }
    }
}

/* Cheap by construction, not by luck: alpha is one value for the whole
 * call, and the Bayer pattern repeats every 4 pixels, so the per-pixel
 * decision collapses to four booleans per row: a fully-covered row is a
 * plain memcpy, an untouched row costs nothing. Phase-locked to absolute
 * panel coordinates like every other dithered draw in draw.h, so
 * overlapping dithered shapes stay in register with each other. First
 * user: the boot animation's photograph crossfade (boot_anim.c's
 * draw_image()). */
void
draw_blit_dither(int x, int y, int w, int h, const gfx_color_t* src, int src_stride, uint8_t alpha) {
    gfx_surface_t state;
    if (!gfx_draw_begin(&state)) {
        return;
    }
    if (alpha == 0) {
        return;
    }

    const recti_t rect = gfx_target_clip_rect(state.target, state.clip, (recti_t){x, y, w, h});
    if (recti_is_empty(rect)) {
        return;
    }

    const int level = gfx_dither_level(alpha);

    for (int row = rect.y; row < recti_bottom(rect); row++) {
        const uint8_t* cells = gfx_dither4x4[row & 3];
        const bool p[4] = {level > cells[0], level > cells[1], level > cells[2], level > cells[3]};

        if (!p[0] && !p[1] && !p[2] && !p[3]) {
            continue;
        }

        gfx_color_t* dst = gfx_target_row(state.target, row);
        const gfx_color_t* s = src + (size_t)(row - y) * (size_t)src_stride + (rect.x - x);

        if (p[0] && p[1] && p[2] && p[3]) {
            memcpy(dst + rect.x, s, (size_t)rect.w * sizeof *dst);
            continue;
        }

        blit_dither_row(dst + rect.x, s, rect.x, recti_right(rect), p);
    }

    if (state.marks) {
        gfx_mark_rows_dirty(rect.y, recti_bottom(rect));
    }
}

/*
 * Text
 *
 * One font-aware path (draw_text_font(), draw_font_text_width()) everything else
 * delegates to, passing draw_font_ui(). See draw_font.h for why draw_font_t
 * exists: honouring microui's mu_Font is a later task needing a font to
 * point AT. draw_font_ui() (draw_font_roles.h) wraps font8x8_basic.h's
 * public-domain bitmap data (draw_font_8x8's comment, draw_font.h).
 */

void
draw_text(int x, int y, const char* text, gfx_color_t color) {
    draw_text_scaled(x, y, text, color, DRAW_GLYPH_SCALE);
}

void
draw_text_scaled(int x, int y, const char* text, gfx_color_t color, int scale) {
    draw_text_turned(x, y, text, color, scale, 0);
}

/* Draws `font` glyph or nothing if `ch` is out of range. */
static inline __attribute__((always_inline)) void
glyph_font(const gfx_surface_t* state, const draw_font_t* font, int x, int y, unsigned char ch, gfx_color_t color,
           int scale, int turn, bool halo) {
    /* Coalesced run boxes merge identical runs across rows, so a vertical
     * stroke needs one target fill regardless of which glyph axis a turn
     * maps onto the screen's narrow one. */
    draw_font_run_box_t boxes[DRAW_FONT_RUN_BOXES_MAX];
    const int n = draw_font_glyph_run_boxes(font, ch, boxes, DRAW_FONT_RUN_BOXES_MAX);
    for (int i = 0; i < n; i++) {
        int rx, ry, rw, rh;
        if (halo) {
            draw_font_run_box_rect_dilated(font, x, y, boxes[i].row0, boxes[i].row1, boxes[i].col0, boxes[i].col1,
                                           scale, turn, &rx, &ry, &rw, &rh);
        } else {
            draw_font_run_box_rect(font, x, y, boxes[i].row0, boxes[i].row1, boxes[i].col0, boxes[i].col1, scale, turn,
                                   &rx, &ry, &rw, &rh);
        }

        const recti_t painted = gfx_target_fill_rect(state->target, state->clip, (recti_t){rx, ry, rw, rh}, color);
        mark_fill(state, painted);
    }
}

static const int text_step[4][2] = {{1, 0}, {0, 1}, {-1, 0}, {0, -1}};

static inline bool
text_entry(gfx_surface_t* state, int* scale, int* turn) {
    if (!gfx_draw_begin(state)) {
        return false;
    }
    *scale = mathi_max(*scale, 1);
    *turn = ((*turn % 4) + 4) % 4;
    return true;
}

static inline __attribute__((always_inline)) void
text_font(int x, int y, const char* text, gfx_color_t color, int scale, int quarter_turns, const draw_font_t* font,
          bool halo) {
    gfx_surface_t state;
    int turn = quarter_turns;
    if (!text_entry(&state, &scale, &turn)) {
        return;
    }

    /* A quarter turn of 1 or 3 swaps which cell dimension becomes the
     * on-screen row extent; see draw_font_row_run_rect()'s own comment. */
    const int char_h = (turn & 1) ? font->cell_w * scale : font->cell_h * scale;
    const int pad = halo ? 1 : 0;

    for (const char* p = text; *p != '\0'; p++) {
        const unsigned char ch = (unsigned char)*p;
        /* One command replayed into several bands (ui_replay_band()) walks
         * every character again per band; skipping one whose own row
         * extent misses the current target entirely turns that back into
         * one walk's worth of work overall, the same as it costs in
         * GFX_LAYOUT_FULL_FB, where the target spans the full screen and
         * this is never false. */
        if (gfx_target_row_range_overlaps(state.target, y - pad, y + char_h + pad)) {
            glyph_font(&state, font, x, y, ch, color, scale, turn, halo);
        }
        const int adv = draw_font_advance(font, ch, scale);
        x += text_step[turn][0] * adv;
        y += text_step[turn][1] * adv;
    }
}

void
draw_text_font(int x, int y, const char* text, gfx_color_t color, int scale, int quarter_turns,
               const draw_font_t* font) {
    text_font(x, y, text, color, scale, quarter_turns, font, false);
}

void
draw_text_turned(int x, int y, const char* text, gfx_color_t color, int scale, int quarter_turns) {
    draw_text_font(x, y, text, color, scale, quarter_turns, draw_font_ui());
}

void
draw_text_font_halo(int x, int y, const char* text, gfx_color_t color, int scale, int quarter_turns,
                    const draw_font_t* font) {
    text_font(x, y, text, color, scale, quarter_turns, font, true);
}

/* Separate dither glyphs keep every opaque text call free of an alpha == 255
 * test in glyph_font(). */
static void
rotated_font_pixel_dither(const gfx_surface_t* state, const draw_font_t* font, int x, int y, int row, int col,
                          int scale, int turn, gfx_color_t color, uint8_t alpha) {
    int px, py;
    switch (turn) {
        case 1:
            px = font->cell_h - 1 - row;
            py = col;
            break;
        case 2:
            px = font->cell_w - 1 - col;
            py = font->cell_h - 1 - row;
            break;
        case 3:
            px = row;
            py = font->cell_w - 1 - col;
            break;
        default:
            px = col;
            py = row;
            break;
    }
    fill_rect_alpha(state, x + px * scale, y + py * scale, scale, scale, color, alpha, false);
}

static void
glyph_font_dither(const gfx_surface_t* state, const draw_font_t* font, int x, int y, unsigned char ch,
                  gfx_color_t color, int scale, int turn, uint8_t alpha) {
    if (ch < font->first || (unsigned)(ch - font->first) >= font->count) {
        return;
    }

    const uint8_t* glyph = font->atlas + (size_t)(ch - font->first) * font->cell_h;

    for (int row = 0; row < font->cell_h; row++) {
        const uint8_t bits = glyph[row];
        if (bits == 0) {
            continue;
        }
        for (int col = 0; col < font->cell_w; col++) {
            if (bits & (1 << col)) {
                rotated_font_pixel_dither(state, font, x, y, row, col, scale, turn, color, alpha);
            }
        }
    }
}

/* draw_text_font() with dithered glyphs for translucent effect. Used in
 * boot_anim.c for title shadow. */
void
draw_text_font_dither(int x, int y, const char* text, gfx_color_t color, int scale, int quarter_turns,
                      const draw_font_t* font, uint8_t alpha) {
    gfx_surface_t state;
    int turn = quarter_turns;
    if (!text_entry(&state, &scale, &turn)) {
        return;
    }

    for (const char* p = text; *p != '\0'; p++) {
        const unsigned char ch = (unsigned char)*p;
        glyph_font_dither(&state, font, x, y, ch, color, scale, turn, alpha);
        const int adv = draw_font_advance(font, ch, scale);
        x += text_step[turn][0] * adv;
        y += text_step[turn][1] * adv;
    }
}
