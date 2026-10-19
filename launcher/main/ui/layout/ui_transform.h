/*
 * ui_transform: viewport quarter turns, renderer axis checks, and microui,
 * text and icon adapters over math/linear/affine2x.h.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "draw/draw_font.h"
#include "draw/icon.h"
#include "math/linear/affine2x.h"
#include "microui.h"

static inline recti_t
ui_recti_from_mu(mu_Rect r) {
    return (recti_t){r.x, r.y, r.w, r.h};
}

static inline mu_Rect
ui_mu_from_recti(recti_t r) {
    return (mu_Rect){r.x, r.y, r.w, r.h};
}

/* Rotation by `turn` quarter turns (mod 4, negative allowed), about a
 * viewport `viewport_w` x `viewport_h` wide: the PHYSICAL panel,
 * unchanged in size when the UI turns. The domain expected is the
 * LOGICAL canvas: for an odd turn that is viewport_h wide, viewport_w
 * tall (see ui_width()/ui_height()), for turn 0 or 2 it is viewport_w x
 * viewport_h. Each matrix entry is exactly 0, MATHX_ONE or -MATHX_ONE,
 * translations exact integers scaled by MATHX_ONE, exact in Q16.16, no
 * rounding to compound. */
static inline affine2x_t
ui_transform_quarter_turn(int turn, int viewport_w, int viewport_h) {
    const int t = ((turn % 4) + 4) % 4;
    const int32_t w = (int32_t)viewport_w << MATHX_SHIFT;
    const int32_t h = (int32_t)viewport_h << MATHX_SHIFT;

    switch (t) {
        case 1: /* one quarter turn clockwise */ return (affine2x_t){{0, MATHX_ONE}, {-MATHX_ONE, 0}, {w, 0}};
        case 2: /* half turn */ return (affine2x_t){{-MATHX_ONE, 0}, {0, -MATHX_ONE}, {w, h}};
        case 3: /* three quarter turns clockwise (one counter-clockwise) */
            return (affine2x_t){{0, -MATHX_ONE}, {MATHX_ONE, 0}, {0, h}};
        default: return affine2x_identity();
    }
}

/* Which ui_transform_quarter_turn() this is, 0 to 3, or 0 for a transform
 * that is not one. */
static inline int
ui_transform_quarter_turns(affine2x_t t) {
    if (t.x_axis.x == 0 && t.y_axis.y == 0) {
        return t.x_axis.y > 0 ? 1 : 3;
    }
    return t.x_axis.x < 0 ? 2 : 0;
}

/* The logical pixel `t` maps onto physical `p`: `p` through t's inverse, or
 * `p` unmapped if `t` is singular. Every transform that passes
 * ui_transform_is_axis_preserving() has an inverse, so the fallback only
 * guards a broken invariant. */
static inline vec2i_t
ui_transform_unmap_pixel(affine2x_t t, vec2i_t p) {
    affine2x_t inverse;
    return affine2x_invert(t, &inverse) ? affine2x_map_pixel(&inverse, p) : p;
}

static inline mu_Rect
ui_transform_rect(affine2x_t t, mu_Rect r) {
    return ui_mu_from_recti(affine2x_map_recti(&t, ui_recti_from_mu(r)));
}

/* icon_walk_blocks()'s callback context, adapted to transform each run
 * before it reaches the caller's own `emit`; see ui_transform_icon_blocks()
 * below for why this exists instead of transforming `box` once up front. */
typedef struct {
    affine2x_t t;
    int box_x, box_y;
    icon_emit_fn emit;
    void* ctx;
} ui_transform_icon_ctx_t;

static inline void
ui_transform_icon_emit(void* ctx, recti_t rect) {
    const ui_transform_icon_ctx_t* ic = ctx;
    rect.x += ic->box_x;
    rect.y += ic->box_y;
    const mu_Rect local = ui_mu_from_recti(rect);
    const mu_Rect dst = ui_transform_rect(ic->t, local);
    ic->emit(ic->ctx, ui_recti_from_mu(dst));
}

/* Every icon run must turn with its logical box; turning only the box
 * places the icon correctly but leaves the glyph upright. */
static inline void
ui_transform_icon_blocks(affine2x_t t, const draw_image_t* icon, mu_Rect box, icon_emit_fn emit, void* ctx) {
    ui_transform_icon_ctx_t ic = {.t = t, .box_x = box.x, .box_y = box.y, .emit = emit, .ctx = ctx};
    icon_walk_blocks(icon, box.w, box.h, ui_transform_icon_emit, &ic);
}

/* Which of draw_text_turned()'s four quarters `t` represents, for a
 * caller that needs to draw text through it; see ui.c's
 * draw_command(). Only the SIGN pattern of the linear part is read, not
 * its magnitude, so this still answers correctly under a scaled
 * transform (see affine2x_mul() with a quarter turn folded into
 * a scale). Meaningless if ui_transform_is_axis_preserving(t) is false;
 * callers are expected to check that first, exactly as draw_command()
 * does. */
static inline int
ui_transform_quarter(affine2x_t t) {
    if (t.x_axis.x > 0 && t.y_axis.y > 0) {
        return 0;
    }
    if (t.x_axis.y > 0 && t.y_axis.x < 0) {
        return 1;
    }
    if (t.x_axis.x < 0 && t.y_axis.y < 0) {
        return 2;
    }
    if (t.x_axis.y < 0 && t.y_axis.x > 0) {
        return 3;
    }
    return 0; /* not a rotation at all (e.g. the zero matrix): identity is
                 the least wrong answer, and
                 ui_transform_is_axis_preserving() would have already
                 rejected this transform anyway. */
}

/* Where draw_text_font()'s (x, y) origin (the FIRST glyph's cell,
 * whichever way the string is about to be drawn) must be placed so a
 * string measuring `box` draws flush inside it at `quarter` turns.
 * Turns 0 and 1 walk FORWARD from the origin, so box's own (x, y)
 * corner already IS where glyph 0 belongs: no correction, none
 * applied. */
static inline void
ui_text_glyph0_origin(const draw_font_t* font, recti_t box, int quarter, int scale, int* out_x, int* out_y) {
    *out_x = box.x;
    *out_y = box.y;
    /* Turns 2 and 3 walk BACKWARD, so glyph 0 starts ONE GLYPH CELL in
     * from the box's FAR edge. That step is `font->cell_w` (NOT
     * cell_h, NOT advance) in BOTH turn 2 (X) and turn 3 (Y): rotation
     * always maps a glyph's COLUMN axis (span cell_w) onto whichever
     * screen axis the string walks along, so glyph 0's footprint is
     * cell_w in every turn, never cell_h. Verified three ways
     * (symbolically, via ui_transform_rect(), pixel-for-pixel against a
     * non-square font); see suite_ui_transform.c. */
    if (quarter == 2) {
        *out_x = box.x + box.w - font->cell_w * scale;
    } else if (quarter == 3) {
        *out_y = box.y + box.h - font->cell_w * scale;
    }
}

/* Whether `t` maps axis-aligned rects to axis-aligned rects: the
 * condition under which draw_rect(), draw_set_clip() and the dirty
 * tracker can be trusted, and draw_text_turned() gets a turn to give.
 * Each logical axis maps onto ONE physical axis, not a mix: either the
 * x/y columns keep to their axis (b == c == 0, unrotated, scaled) or
 * swap cleanly (a == d == 0, a 90/270 turn). A shear or arbitrary
 * rotation has a nonzero entry in both columns. `!= 0` rules out a zero
 * scale, not invertible. */
static inline bool
ui_transform_is_axis_preserving(affine2x_t t) {
    if (t.x_axis.y == 0 && t.y_axis.x == 0) {
        return t.x_axis.x != 0 && t.y_axis.y != 0;
    }
    if (t.x_axis.x == 0 && t.y_axis.y == 0) {
        return t.x_axis.y != 0 && t.y_axis.x != 0;
    }
    return false;
}
