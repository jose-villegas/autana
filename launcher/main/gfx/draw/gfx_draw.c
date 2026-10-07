#include "gfx/draw/gfx_draw.h"
#include "gfx/gfx_internal.h"
#include "gfx/present/gfx_fb_guard.h"
#include "gfx/present/gfx_present.h"
#include "gfx/present/gfx_present_guard.h"
#include "util/runtime/tune.h"
#include "util/scalar/mathi.h"

#include <string.h>

/* Current clip rectangle, as inclusive-exclusive bounds. */
static gfx_box_t clip;

TUNE_OWNER(gfx);

/* 1 hands the tracker the box a fill has already clipped; 0 claims only
 * its rows, every column at full width, which is all mark_band() can
 * express. The band path's skip test (dirty_band_extent(), gfx_dirty.h)
 * reads a cell's narrowed y as well as its x, so a band no narrowed cell
 * overlaps is skipped; SET gfx.tight_fill_marks 0 separates that from a
 * fault in an app's own marking, on the device, without a reflash. */
TUNE(gfx, tight_fill_marks, 1, 0, 1);

/* What every rect and blit primitive marks with. A release build folds
 * tight_fill_marks to the constant 1 (util/runtime/tune.h), so the branch and
 * mark_band() call below cost nothing there. */
static inline void
mark_fill(int x0, int y0, int x1, int y1) {
    if (band_render_active) {
        return;
    }
    if (tight_fill_marks) {
        dirty_mark(x0, y0, x1 - x0, y1 - y0);
    } else {
        mark_band(y0, y1);
    }
}

/* Colour */

/* Pack 0xRRGGBB to RGB565, byte-swapped: the panel takes the high byte
 * first, the CPU stores the low byte first, and DMA sends memory as it lies. */
gfx_color_t
gfx_rgb(uint32_t rgb) {
    return GFX_RGB(rgb);
}

/* Clipping */

void
gfx_set_clip(int x, int y, int w, int h) {
    GFX_PRESENT_GUARD();
    int x1 = x + w;
    int y1 = y + h;

    clip.x0 = x < 0 ? 0 : x;
    clip.y0 = y < 0 ? 0 : y;
    clip.x1 = x1 > GFX_WIDTH ? GFX_WIDTH : x1;
    clip.y1 = y1 > GFX_HEIGHT ? GFX_HEIGHT : y1;
}

void
gfx_clear_clip(void) {
    GFX_PRESENT_GUARD();
    clip.x0 = 0;
    clip.y0 = 0;
    clip.x1 = GFX_WIDTH;
    clip.y1 = GFX_HEIGHT;
}

/* Primitives */

/* Ignores clip rect; clears the whole target (a bounding box in full-fb
 * mode's partial-clear path, or the whole target buffer otherwise) and
 * marks it dirty: dirty tracking is meaningless while a band is the
 * target, since band mode resends every band every frame regardless, so
 * that half is skipped entirely there. */
void
gfx_clear(gfx_color_t color) {
    GFX_PRESENT_GUARD();
    if (!GFX_REQUIRE_FRAMEBUFFER()) {
        return;
    }
    if (!band_render_active && partial_clear_on && !gfx_box_is_empty(prev_bbox)) {
        for (int y = prev_bbox.y0; y < prev_bbox.y1; y++) {
            gfx_color_t* dst = fb + (size_t)y * GFX_WIDTH + prev_bbox.x0;
            for (int x = prev_bbox.x0; x < prev_bbox.x1; x++) {
                *dst++ = color;
            }
        }
        dirty_mark(prev_bbox.x0, prev_bbox.y0, prev_bbox.x1 - prev_bbox.x0, prev_bbox.y1 - prev_bbox.y0);
        drawn_bbox = GFX_BOX_EMPTY;
        return;
    }

    const gfx_target_t target = current_target();
    const uint32_t pair = ((uint32_t)color << 16) | color;
    uint32_t* words = (uint32_t*)target.buf;
    const int count = (target.stride * target.height) / 2;

    for (int i = 0; i < count; i++) {
        words[i] = pair;
    }

    if (!band_render_active) {
        gfx_mark_all_dirty();
    }
}

void
gfx_pixel(int x, int y, gfx_color_t color) {
    GFX_PRESENT_GUARD();
    if (!GFX_REQUIRE_FRAMEBUFFER()) {
        return;
    }
    if (x < clip.x0 || x >= clip.x1) {
        return;
    }
    const gfx_target_t target = current_target();
    const gfx_box_t rect = gfx_target_clip_rect(target, clip, (gfx_box_t){x, y, x + 1, y + 1});
    if (rect.y0 >= rect.y1) {
        return;
    }
    gfx_target_row(target, y)[x] = color;
    if (!band_render_active) {
        mark_band(y, y + 1);
    }
}

/* One pixel of a line. */
static void
plot(int x, int y, gfx_color_t color, unsigned flags) {
    if (x < clip.x0 || x >= clip.x1) {
        return;
    }
    const gfx_target_t target = current_target();
    const gfx_box_t rect = gfx_target_clip_rect(target, clip, (gfx_box_t){x, y, x + 1, y + 1});
    if (rect.y0 >= rect.y1) {
        return;
    }
    gfx_color_t* const dst = &gfx_target_row(target, y)[x];

    *dst = (flags & GFX_LINE_ADD) ? gfx_color_add(*dst, color) : color;
}

/* Bresenham, treats both axes alike, no case analysis. */
static void
walk(int x0, int y0, int x1, int y1, gfx_color_t color, unsigned flags) {
    const int dx = mathi_abs(x1 - x0);
    const int dy = -mathi_abs(y1 - y0);
    const int sx = x0 < x1 ? 1 : -1;
    const int sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;
    bool first = true;

    for (;;) {
        if (!(first && (flags & GFX_LINE_OPEN))) {
            plot(x0, y0, color, flags);
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

/* Box intersects clip, marked once. Overestimating costs bus time;
 * underestimating leaves stale. */
static void
draw_line(int x0, int y0, int x1, int y1, gfx_color_t color, unsigned flags) {
    if (!gfx_box_clip_segment(&clip, &x0, &y0, &x1, &y1)) {
        return;
    }

    int bx0 = mathi_min(x0, x1), bx1 = mathi_max(x0, x1) + 1;
    int by0 = mathi_min(y0, y1), by1 = mathi_max(y0, y1) + 1;

    if (bx0 < clip.x0) {
        bx0 = clip.x0;
    }
    if (by0 < clip.y0) {
        by0 = clip.y0;
    }
    if (bx1 > clip.x1) {
        bx1 = clip.x1;
    }
    if (by1 > clip.y1) {
        by1 = clip.y1;
    }

    walk(x0, y0, x1, y1, color, flags);

    if (!band_render_active && bx0 < bx1 && by0 < by1) {
        dirty_mark(bx0, by0, bx1 - bx0, by1 - by0);
    }
}

void
gfx_line(int x0, int y0, int x1, int y1, gfx_color_t color) {
    GFX_PRESENT_GUARD();
    if (!GFX_REQUIRE_FRAMEBUFFER()) {
        return;
    }
    draw_line(x0, y0, x1, y1, color, 0);
}

void
gfx_line_ex(int x0, int y0, int x1, int y1, gfx_color_t color, unsigned flags) {
    GFX_PRESENT_GUARD();
    if (!GFX_REQUIRE_FRAMEBUFFER()) {
        return;
    }
    draw_line(x0, y0, x1, y1, color, flags);
}

void
gfx_fill_rect(int x, int y, int w, int h, gfx_color_t color) {
    GFX_PRESENT_GUARD();
    if (!GFX_REQUIRE_FRAMEBUFFER()) {
        return;
    }
    int x0, y0, x1, y1;
    gfx_target_fill_rect(current_target(), clip.x0, clip.y0, clip.x1, clip.y1, x, y, w, h, color, &x0, &y0, &x1, &y1);

    mark_fill(x0, y0, x1, y1);
}

/*
 * Dithered fake transparency
 *
 * gfx_fill_rect_blend() (further down) is a REAL per-pixel blend, but pays
 * for a framebuffer read, affordable at glyph scale, not a whole frame
 * (its own comment). Dithering fakes transparency instead: ordered (Bayer)
 * dithering picks WHICH pixels to draw via a per-pixel threshold, no
 * framebuffer read, no float math.
 */

/* Dither coverage avoids framebuffer reads; blending reads each destination pixel. */
static inline __attribute__((always_inline)) void
fill_rect_alpha(int x, int y, int w, int h, gfx_color_t color, uint8_t alpha, bool blend) {
    GFX_PRESENT_GUARD();
    if (!GFX_REQUIRE_FRAMEBUFFER() || alpha == 0) {
        return;
    }

    const gfx_target_t target = current_target();
    const gfx_box_t rect = gfx_target_clip_rect(target, clip, (gfx_box_t){x, y, x + w, y + h});

    for (int row = rect.y0; row < rect.y1; row++) {
        gfx_color_t* dst = gfx_target_row(target, row);
        for (int col = rect.x0; col < rect.x1; col++) {
            if (blend) {
                dst[col] = gfx_color_mix(dst[col], color, alpha);
            } else if (gfx_dither_covers(col, row, alpha)) {
                dst[col] = color;
            }
        }
    }

    mark_fill(rect.x0, rect.y0, rect.x1, rect.y1);
}

void
gfx_fill_rect_dither(int x, int y, int w, int h, gfx_color_t color, uint8_t alpha) {
    fill_rect_alpha(x, y, w, h, color, alpha, false);
}

void
gfx_fill_rect_blend(int x, int y, int w, int h, gfx_color_t color, uint8_t alpha) {
    fill_rect_alpha(x, y, w, h, color, alpha, true);
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
 * panel coordinates like every other dithered draw in gfx.h, so
 * overlapping dithered shapes stay in register with each other. First
 * user: the boot animation's photograph crossfade (boot_anim.c's
 * draw_image()). */
void
gfx_blit_dither(int x, int y, int w, int h, const gfx_color_t* src, int src_stride, uint8_t alpha) {
    GFX_PRESENT_GUARD();
    if (!GFX_REQUIRE_FRAMEBUFFER()) {
        return;
    }
    if (alpha == 0) {
        return;
    }

    const gfx_target_t target = current_target();
    const gfx_box_t rect = gfx_target_clip_rect(target, clip, (gfx_box_t){x, y, x + w, y + h});
    if (rect.x0 >= rect.x1 || rect.y0 >= rect.y1) {
        return;
    }

    const int level = gfx_dither_level(alpha);

    for (int row = rect.y0; row < rect.y1; row++) {
        const uint8_t* cells = gfx_dither4x4[row & 3];
        const bool p[4] = {level > cells[0], level > cells[1], level > cells[2], level > cells[3]};

        if (!p[0] && !p[1] && !p[2] && !p[3]) {
            continue;
        }

        gfx_color_t* dst = gfx_target_row(target, row);
        const gfx_color_t* s = src + (size_t)(row - y) * (size_t)src_stride + (rect.x0 - x);

        if (p[0] && p[1] && p[2] && p[3]) {
            memcpy(dst + rect.x0, s, (size_t)(rect.x1 - rect.x0) * sizeof *dst);
            continue;
        }

        blit_dither_row(dst + rect.x0, s, rect.x0, rect.x1, p);
    }

    if (!band_render_active) {
        mark_band(rect.y0, rect.y1);
    }
}

/*
 * Text
 *
 * One font-aware path (gfx_text_font(), gfx_font_width()) everything else
 * delegates to, passing gfx_font_ui(). See gfx_font.h for why gfx_font_t
 * exists: honouring microui's mu_Font is a later task needing a font to
 * point AT. gfx_font_ui() (gfx_font_roles.h) wraps font8x8_basic.h's
 * public-domain bitmap data (gfx_font_8x8's comment, gfx_font.h).
 */

void
gfx_text(int x, int y, const char* text, gfx_color_t color) {
    gfx_text_scaled(x, y, text, color, GFX_GLYPH_SCALE);
}

void
gfx_text_scaled(int x, int y, const char* text, gfx_color_t color, int scale) {
    gfx_text_turned(x, y, text, color, scale, 0);
}

/* Draws `font` glyph or nothing if `ch` is out of range. */
static inline __attribute__((always_inline)) void
draw_glyph_font(const gfx_font_t* font, int x, int y, unsigned char ch, gfx_color_t color, int scale, int turn,
                bool halo) {
    /* One filled rect per coalesced box of set bits, not one per run
     * per row: gfx_font_glyph_run_boxes() merges a vertical stroke's
     * identical run across every row it spans into one box, so
     * gfx_font_run_box_rect() covers it with one gfx_fill_rect() call
     * regardless of which glyph axis a turn maps onto the screen's
     * narrow one. */
    gfx_font_run_box_t boxes[GFX_FONT_RUN_BOXES_MAX];
    const int n = gfx_font_glyph_run_boxes(font, ch, boxes, GFX_FONT_RUN_BOXES_MAX);
    for (int i = 0; i < n; i++) {
        int rx, ry, rw, rh;
        if (halo) {
            gfx_font_run_box_rect_dilated(font, x, y, boxes[i].row0, boxes[i].row1, boxes[i].col0, boxes[i].col1, scale,
                                          turn, &rx, &ry, &rw, &rh);
        } else {
            gfx_font_run_box_rect(font, x, y, boxes[i].row0, boxes[i].row1, boxes[i].col0, boxes[i].col1, scale, turn,
                                  &rx, &ry, &rw, &rh);
        }
        gfx_fill_rect(rx, ry, rw, rh, color);
    }
}

static inline __attribute__((always_inline)) void
draw_text_font(int x, int y, const char* text, gfx_color_t color, int scale, int quarter_turns, const gfx_font_t* font,
               bool halo) {
    GFX_PRESENT_GUARD();
    if (!GFX_REQUIRE_FRAMEBUFFER()) {
        return;
    }
    if (scale < 1) {
        scale = 1;
    }

    const int turn = ((quarter_turns % 4) + 4) % 4;

    static const int step[4][2] = {
        {1, 0},  /* upright:        left to right */
        {0, 1},  /* quarter turn:   top to bottom */
        {-1, 0}, /* upside down:    right to left */
        {0, -1}, /* three quarters: bottom to top */
    };

    /* A quarter turn of 1 or 3 swaps which cell dimension becomes the
     * on-screen row extent; see gfx_font_row_run_rect()'s own comment. */
    const int char_h = (turn & 1) ? font->cell_w * scale : font->cell_h * scale;
    const int pad = halo ? 1 : 0;
    const gfx_target_t target = current_target();

    for (const char* p = text; *p != '\0'; p++) {
        const unsigned char ch = (unsigned char)*p;
        /* One command replayed into several bands (ui_replay_band()) walks
         * every character again per band; skipping one whose own row
         * extent misses the current target entirely turns that back into
         * one walk's worth of work overall, the same as it costs in
         * GFX_LAYOUT_FULL_FB, where the target spans the full screen and
         * this is never false. */
        if (gfx_target_row_range_overlaps(target, y - pad, y + char_h + pad)) {
            draw_glyph_font(font, x, y, ch, color, scale, turn, halo);
        }
        const int adv = gfx_font_advance(font, ch, scale);
        x += step[turn][0] * adv;
        y += step[turn][1] * adv;
    }
}

void
gfx_text_font(int x, int y, const char* text, gfx_color_t color, int scale, int quarter_turns, const gfx_font_t* font) {
    draw_text_font(x, y, text, color, scale, quarter_turns, font, false);
}

void
gfx_text_turned(int x, int y, const char* text, gfx_color_t color, int scale, int quarter_turns) {
    gfx_text_font(x, y, text, color, scale, quarter_turns, gfx_font_ui());
}

void
gfx_text_font_halo(int x, int y, const char* text, gfx_color_t color, int scale, int quarter_turns,
                   const gfx_font_t* font) {
    draw_text_font(x, y, text, color, scale, quarter_turns, font, true);
}

/*
 * A second copy of the three glyph functions rather than one core threaded
 * with an alpha parameter, deliberately: gfx_text_font() is the single
 * font-aware path every text call in the tree goes through, and keeping it
 * provably unchanged beats trusting a compiler to fold an `alpha == 255`
 * check back out of it at every call site forever.
 */

static void
draw_rotated_font_pixel_dither(const gfx_font_t* font, int x, int y, int row, int col, int scale, int turn,
                               gfx_color_t color, uint8_t alpha) {
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
    gfx_fill_rect_dither(x + px * scale, y + py * scale, scale, scale, color, alpha);
}

static void
draw_glyph_font_dither(const gfx_font_t* font, int x, int y, unsigned char ch, gfx_color_t color, int scale, int turn,
                       uint8_t alpha) {
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
                draw_rotated_font_pixel_dither(font, x, y, row, col, scale, turn, color, alpha);
            }
        }
    }
}

/* gfx_text_font() with dithered glyphs for translucent effect. Used in
 * boot_anim.c for title shadow. */
void
gfx_text_font_dither(int x, int y, const char* text, gfx_color_t color, int scale, int quarter_turns,
                     const gfx_font_t* font, uint8_t alpha) {
    GFX_PRESENT_GUARD();
    if (!GFX_REQUIRE_FRAMEBUFFER()) {
        return;
    }
    if (scale < 1) {
        scale = 1;
    }

    const int turn = ((quarter_turns % 4) + 4) % 4;

    static const int step[4][2] = {
        {1, 0},
        {0, 1},
        {-1, 0},
        {0, -1},
    };

    for (const char* p = text; *p != '\0'; p++) {
        const unsigned char ch = (unsigned char)*p;
        draw_glyph_font_dither(font, x, y, ch, color, scale, turn, alpha);
        const int adv = gfx_font_advance(font, ch, scale);
        x += step[turn][0] * adv;
        y += step[turn][1] * adv;
    }
}
