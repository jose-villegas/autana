/*
 * ui_build - builds one microui frame: touch-to-mouse translation, style and
 * font state, and the widget helpers (ui_slider_int(), ui_draw_icon(),
 * styled button frames) an app's own screen calls while doing it. See ui.h
 * for the whole module's why; ui.c is this file's other half, painting the
 * frame ui_build.c just built into the real framebuffer.
 *
 * THE SPLIT, AND WHY IT IS HOST-PORTABLE
 *
 * Everything here stops at describing the picture - mu_draw_rect() and
 * friends append to microui's own command list, never touch a pixel. The one
 * thing that turns a command list into pixels, ui.c's draw_command(), is the
 * only part of the module that cannot link without the real framebuffer and
 * panel. Splitting on exactly that line is what lets an app's own screen -
 * built through the calls this file exports - run on a host, against the
 * real microui and the real font metrics, and be measured or asserted on
 * there instead of only judged by eye on the device.
 */

#include "ui/ui.h"

#include <string.h>

#ifdef DEVICE_BUILD
#include "esp_log.h"
static const char* TAG = "ui";
#endif

#include "gfx/gfx.h"
#include "gfx/gfx_font_roles.h"
#include "ui/ui_internal.h"
#include "ui/ui_pointer.h"
#include "ui/ui_slider.h"

/* Definitions for the externs ui_internal.h declares - see that header for
 * what each one is shared for. */
mu_Context ctx;
bool invalidated = true;
ui_text_style_t text_style;
ui_pointer_t pointer;
uint64_t canvas_hash[MU_CONTAINERPOOL_SIZE];

static ui_button_style_t button_style;
/* The style in force for the rest of this frame, and microui's own frame
 * painter, kept so UI_BUTTON_FLAT and every non-button frame stay exactly
 * what upstream draws. Captured from the context rather than
 * reimplemented here: microui's draw_frame() is static to microui.c, and
 * a hand-copied twin of it would be one more thing to keep in step
 * across a version bump. */
static void (*base_draw_frame)(mu_Context*, mu_Rect, int);

/* The transform every command is mapped through before it is drawn, and
 * whether it is one draw_command() may actually use - see ui_set_transform()
 * below for why an invalid one is remembered rather than rejected outright. */
static ui_transform_t transform;
static bool transform_valid;

/* See ui.h's own comment above ui_layout_generation() for what this counts
 * and, more importantly, what it is not. Bumped in the same branch of
 * ui_set_transform() below that already calls ui_invalidate() - a genuine
 * transform change is the one and only thing that increments it. */
static uint32_t layout_generation;

/* Every (font, scale) pair anyone has asked for, so the same pair always
 * yields the same address - see intern_font_scaled() below for why that
 * stability is load-bearing. Small and never cleared: one shell, one
 * mu_Context, realistically a handful of roles at a handful of scales
 * for the app's whole lifetime, not a per-screen or per-frame set. */
#define UI_FONT_SCALED_MAX 8
static ui_font_scaled_t font_scaled_table[UI_FONT_SCALED_MAX];
static int font_scaled_count;

/* Returns the SAME address for the same (font, scale) every time, which is
 * what lets hash_canvas() (ui.c) notice a scale change on its own, the
 * same way it already does for a font change - see ui_set_font()'s
 * comment. */
static const ui_font_scaled_t*
intern_font_scaled(const gfx_font_t* font, int scale) {
    for (int i = 0; i < font_scaled_count; i++) {
        if (font_scaled_table[i].font == font && font_scaled_table[i].scale == scale) {
            return &font_scaled_table[i];
        }
    }
    if (font_scaled_count < UI_FONT_SCALED_MAX) {
        font_scaled_table[font_scaled_count] = (ui_font_scaled_t){font, scale};
        return &font_scaled_table[font_scaled_count++];
    }
    /* Full: hand back the default pair rather than recycling a slot.
     * Overwriting one retargets every mu_Font already pointing at it - slot 0
     * is the shell default - so the picture changes while the command list
     * keeps the same bytes, which is exactly what hash_canvas() cannot see.
     * Text at the wrong size is visible and recoverable; a canvas that skips
     * its repaint is neither. Raise UI_FONT_SCALED_MAX instead. */
#ifdef DEVICE_BUILD
    ESP_LOGW(TAG, "font/scale table full (%d entries) - falling back to the default", UI_FONT_SCALED_MAX);
#endif
    return &font_scaled_table[0];
}

/* mu_Font is NULL only before ui_init() has run - see measure_text_width()/
 * height() below, which is where that matters. */
ui_font_scaled_t
ui_resolve_font_scaled(mu_Font font) {
    if (font) {
        return *(const ui_font_scaled_t*)font;
    }
    return (ui_font_scaled_t){gfx_font_ui(), GFX_GLYPH_SCALE};
}

/* microui asks us for text metrics rather than measuring anything itself.
 * `font` is whatever ctx.style->font held when the widget that wants
 * metrics ran - see ui_set_font() in ui.h. Falling back rather than
 * dereferencing NULL means a widget measured before ui_init() gets a
 * sane answer instead of a crash. */
static int
measure_text_width(mu_Font font, const char* str, int len) {
    const ui_font_scaled_t fs = ui_resolve_font_scaled(font);
    return gfx_font_text_width(fs.font, str, len, fs.scale);
}

static int
measure_text_height(mu_Font font) {
    const ui_font_scaled_t fs = ui_resolve_font_scaled(font);
    return gfx_font_height(fs.font, fs.scale);
}

/*
 * Why the pressed look keys off hover and not only focus: on a mouse, hover
 * means the pointer is near and focus means the button is held, but touch
 * has neither until contact, so hover IS contact. Focus covers most of a
 * tap, yet the one synthesized hover frame before DOWN lands has no focus
 * yet. See ui_style.h for what a style is and why it produces spans.
 */

static bool
is_button_frame(int colorid) {
    return colorid == MU_COLOR_BUTTON || colorid == MU_COLOR_BUTTONHOVER || colorid == MU_COLOR_BUTTONFOCUS;
}

static void
styled_draw_frame(mu_Context* c, mu_Rect rect, int colorid) {
    if (button_style != UI_BUTTON_BEZEL || !is_button_frame(colorid)) {
        base_draw_frame(c, rect, colorid);
        return;
    }

    ui_span_t spans[UI_BEZEL_MAX_SPANS];
    const int n =
        ui_bezel_spans(rect, c->style->colors[colorid], colorid != MU_COLOR_BUTTON, spans, UI_BEZEL_MAX_SPANS);
    for (int i = 0; i < n; i++) {
        mu_draw_rect(c, spans[i].rect, spans[i].color);
    }
}

void
ui_set_button_style(ui_button_style_t style) {
    button_style = style;
}

/* WHY THIS NEEDS ui_invalidate() AND ui_set_button_style() DOES NOT: a
 * bezel is real mu_draw_rect() commands, so a style change is a content
 * change ui_end()'s hash sees. Text style applies at RENDER time inside
 * draw_command() - the command list is byte-identical either way, so
 * hash_canvas() can't see it and the repaint is skipped, leaving OLD
 * pixels under the new intent. Do NOT delete this call "for consistency"
 * - the two are not symmetric, and deleting it reintroduces the bug it
 * prevents. */
void
ui_set_text_style(ui_text_style_t style) {
    if (style != text_style) {
        text_style = style;
        ui_invalidate();
    }
}

/* WHY THIS NEEDS NO ui_invalidate(), UNLIKE ui_set_text_style() ABOVE:
 * mu_Font rides inside every mu_TextCommand, so a font (or scale) change
 * is different bytes and hash_canvas() sees it unaided. THAT property is
 * exactly why the scale lives here too rather than a render-time global -
 * a global would need invalidating on every size change, which a two-size
 * screen hits every frame, permanently defeating the repaint skip. */
void
ui_set_font_scaled(const gfx_font_t* font, int scale) {
    if (scale < 1) {
        scale = 1;
    }
    ctx.style->font = (mu_Font)intern_font_scaled(font ? font : gfx_font_ui(), scale);
}

void
ui_set_font(const gfx_font_t* font) {
    ui_set_font_scaled(font, GFX_GLYPH_SCALE);
}

static bool
transforms_equal(ui_transform_t a, ui_transform_t b) {
    return a.a == b.a && a.b == b.b && a.c == b.c && a.d == b.d && a.tx == b.tx && a.ty == b.ty;
}

/* See ui.h for why a transform change must call ui_invalidate(). WHY AN
 * INVALID TRANSFORM IS REMEMBERED RATHER THAN REJECTED. ui_transform_t can
 * express more than this renderer can draw - see ui_transform.h. Logging at
 * set time, not render time, keeps this to one log line, not one per frame. */
void
ui_set_transform(ui_transform_t t) {
    if (transforms_equal(t, transform)) {
        return;
    }
    transform = t;
    transform_valid = ui_transform_is_axis_preserving(t);
    /* Past transforms_equal()'s early return, so this IS a genuine change -
     * see ui_layout_generation()'s comment in ui.h for what counts as one
     * and why this is the only place that gets to bump it. */
    layout_generation++;
    if (!transform_valid) {
#ifdef DEVICE_BUILD
        ESP_LOGE(TAG, "ui_set_transform: transform is not a rotation by a "
                      "multiple of 90 degrees, translation or scale - this "
                      "renderer cannot draw it, so identity will be used until a "
                      "valid transform is set");
#endif
    }
    ui_invalidate();
}

/* What draw_command() (ui.c), feed_input() and ui_width()/ui_height() below
 * actually use: the transform in force, or identity for as long as it fails
 * ui_transform_is_axis_preserving() - see ui_set_transform() above. */
ui_transform_t
ui_effective_transform(void) {
    return transform_valid ? transform : ui_transform_identity();
}

mu_Context*
ui_context(void) {
    return &ctx;
}

uint32_t
ui_layout_generation(void) {
    return layout_generation;
}

void
ui_invalidate(void) {
    invalidated = true;
}

void
ui_init(void) {
    mu_init(&ctx);
    ctx.text_width = measure_text_width;
    ctx.text_height = measure_text_height;
    font_scaled_count = 0;
    ui_set_font(gfx_font_ui());

    base_draw_frame = ctx.draw_frame;
    ctx.draw_frame = styled_draw_frame;
    button_style = UI_BUTTON_FLAT;
    text_style = UI_TEXT_PLAIN;
    transform = ui_transform_identity();
    transform_valid = true;
    /* Explicit, not left to a zeroed static's implicit value - see
     * ui_layout_generation()'s comment in ui.h. 0 is simply the first value
     * a monotonic counter can have; nothing reads meaning into it beyond
     * "no genuine transform change has happened yet this run". */
    layout_generation = 0;

    /* Palette. Deliberately dark: this is an OLED, so black pixels are off
     * pixels - it costs less power and looks better than a grey chrome. */
    ctx.style->colors[MU_COLOR_WINDOWBG] = (mu_Color){0x0A, 0x0C, 0x14, 255};
    ctx.style->colors[MU_COLOR_TEXT] = (mu_Color){0xE6, 0xEA, 0xF2, 255};
    ctx.style->colors[MU_COLOR_BUTTON] = (mu_Color){0x16, 0x1A, 0x28, 255};
    ctx.style->colors[MU_COLOR_BUTTONHOVER] = (mu_Color){0x23, 0x2A, 0x40, 255};
    ctx.style->colors[MU_COLOR_BUTTONFOCUS] = (mu_Color){0x3D, 0xDC, 0x97, 255};
    ctx.style->padding = 12;
    ctx.style->spacing = UI_ROW_GAP;
    ctx.style->indent = 0;
    ctx.style->title_height = UI_TITLE_HEIGHT;

    memset(canvas_hash, 0, sizeof(canvas_hash));
    invalidated = true;
}

/*
 * mu_update_control() takes hover only on a frame where the button is NOT
 * held, and submits only once it has focus - the mouse's "point, then
 * click". A touchscreen has no such sequence, so the missing frame is
 * synthesised: on the press, deliver the position alone and hold
 * button-down for the following frame.
 *
 * Touch also arrives in PHYSICAL coordinates while controls were laid out in
 * LOGICAL ones, so a tap needs the inverse transform first.
 */

/* Also where ui_pointer_step()'s off-screen park point (-1, -1) gets mapped:
 * under a translating transform, logical "off-screen" is not necessarily
 * (-1, -1) either, so the park needs the same inverse as a real touch to
 * stay outside whatever the logical canvas currently is. */
static void
to_logical(int x, int y, int* lx, int* ly) {
    ui_transform_t inv;
    if (!ui_transform_invert(ui_effective_transform(), &inv)) {
        /* Unreachable in practice: ui_effective_transform() is always
         * identity or something ui_transform_is_axis_preserving() accepted,
         * and every transform that passes that check has a nonzero
         * determinant, hence an inverse. Fall back to an unmapped point
         * rather than garbage if that invariant is ever broken. */
        *lx = x;
        *ly = y;
        return;
    }
    ui_transform_point(inv, x, y, lx, ly);
}

/* One ui_pointer_t event, mapped to logical and replayed into microui. The
 * policy itself - hover, then hold down until the real release, park
 * off-screen when idle - lives in ui_pointer_step(); this only translates
 * and dispatches what it returns. */
static void
replay_pointer_event(const ui_pointer_event_t* e) {
    int lx, ly;
    to_logical(e->x, e->y, &lx, &ly);

    switch (e->kind) {
        case UI_POINTER_MOVE: mu_input_mousemove(&ctx, lx, ly); break;
        case UI_POINTER_DOWN: mu_input_mousedown(&ctx, lx, ly, MU_MOUSE_LEFT); break;
        case UI_POINTER_UP: mu_input_mouseup(&ctx, lx, ly, MU_MOUSE_LEFT); break;
        case UI_POINTER_SCROLL: {
            /* A distance, not a point: only the transform's turn applies. */
            int ox, oy;
            to_logical(0, 0, &ox, &oy);
            mu_input_scroll(&ctx, lx - ox, ly - oy);
            break;
        }
    }
}

static void
feed_input(const input_t* input) {
    ui_pointer_event_t events[UI_POINTER_MAX_EVENTS];
    const int n = ui_pointer_step(&pointer, input, events, UI_POINTER_MAX_EVENTS);
    for (int i = 0; i < n; i++) {
        replay_pointer_event(&events[i]);
    }
}

void
ui_begin(const input_t* input) {
    /* Reset before the caller can state its own - see ui.h on why style does
     * not persist across frames. */
    button_style = UI_BUTTON_FLAT;
    feed_input(input);
    mu_begin(&ctx);
}

/* See ui.h: the physical viewport mapped through the inverse transform. Both
 * go through one shared computation since a rect's width and height are just
 * as entangled by a quarter turn as its x and y are - deriving them
 * separately would mean inverting the transform twice for one answer. */
static mu_Rect
logical_viewport(void) {
    ui_transform_t inv;
    if (!ui_transform_invert(ui_effective_transform(), &inv)) {
        /* See to_logical()'s identical fallback above: unreachable while
         * ui_effective_transform() only ever returns identity or a
         * transform that already passed ui_transform_is_axis_preserving(),
         * both of which are invertible by construction. */
        return (mu_Rect){0, 0, GFX_WIDTH, GFX_HEIGHT};
    }
    return ui_transform_rect(inv, (mu_Rect){0, 0, GFX_WIDTH, GFX_HEIGHT});
}

int
ui_width(void) {
    return logical_viewport().w;
}

int
ui_height(void) {
    return logical_viewport().h;
}

int
ui_measure_text(const char* str) {
    const ui_font_scaled_t fs = ui_resolve_font_scaled(ctx.style->font);
    return gfx_font_text_width(fs.font, str, -1, fs.scale);
}

/* icon_walk_blocks()'s callback context: everything one emitted run needs to
 * become a mu_draw_rect() call, and nothing else - kept off the stack as an
 * array only, never grown into a buffer. */
typedef struct {
    mu_Context* c;
    mu_Rect r;
    mu_Color color;
} ui_draw_icon_ctx_t;

static void
ui_draw_icon_emit(void* ctx_, int x, int y, int w, int h) {
    const ui_draw_icon_ctx_t* dc = ctx_;
    mu_draw_rect(dc->c, mu_rect(dc->r.x + x, dc->r.y + y, w, h), dc->color);
}

void
ui_draw_icon(mu_Context* c, mu_Rect r, const icon_t* icon, const uint8_t* rows, mu_Color color) {
    ui_draw_icon_ctx_t dc = {.c = c, .r = r, .color = color};
    icon_walk_blocks(rows + icon->offset, icon->w, icon->h, icon->stride, r.w, r.h, ui_draw_icon_emit, &dc);
}

/* See ui.h. `value`'s own address (not what it points to, same idiom
 * mu_slider_ex() uses) gives each call site a stable id with no string
 * needed. MU_OPT_HOLDFOCUS keeps a drag updating once it leaves the knob. */
bool
ui_slider_int(mu_Context* c, int* value, int lo, int hi, int step) {
    const mu_Id id = mu_get_id(c, &value, sizeof(value));
    const mu_Rect track = mu_layout_next(c);
    mu_update_control(c, id, track, MU_OPT_HOLDFOCUS);

    int v = *value;
    if (c->focus == id && (c->mouse_down | c->mouse_pressed) == MU_MOUSE_LEFT) {
        v = ui_slider_value_at_x(track, lo, hi, UI_SLIDER_KNOB_W, step, c->mouse_pos.x);
    }
    v = mu_clamp(v, lo, hi);
    const bool changed = (v != *value);
    *value = v;

    ui_span_t panel[UI_PANEL_MAX_SPANS];
    const int pn = ui_panel_spans(track, c->style->colors[MU_COLOR_BASE], c->style->colors[MU_COLOR_BORDER], panel,
                                  UI_PANEL_MAX_SPANS);
    if (pn > 0) {
        /* Face, then the filled portion, then the border last - so the
         * border still frames the whole track rather than the fill
         * painting over it where the two overlap. */
        mu_draw_rect(c, panel[0].rect, panel[0].color);
        mu_draw_rect(c, ui_slider_fill_rect(track, lo, hi, v, UI_SLIDER_KNOB_W),
                     c->style->colors[MU_COLOR_BUTTONFOCUS]);
        for (int i = 1; i < pn; i++) {
            mu_draw_rect(c, panel[i].rect, panel[i].color);
        }

        ui_span_t knob[UI_BEZEL_MAX_SPANS];
        const mu_Rect knob_rect = ui_slider_knob_rect(track, lo, hi, v, UI_SLIDER_KNOB_W);
        const int kn = ui_bezel_spans(knob_rect, c->style->colors[MU_COLOR_BUTTON], false, knob, UI_BEZEL_MAX_SPANS);
        for (int i = 0; i < kn; i++) {
            mu_draw_rect(c, knob[i].rect, knob[i].color);
        }
    }

    return changed;
}

/* See ui.h for the full argument. Short version: mu_begin_window_ex()
 * only seeds cnt->rect the FIRST time a title is opened, remembering it
 * forever after - correct for a desktop window manager, wrong here,
 * where a rect must track ui_width()/ui_height() every frame. So this
 * always passes the current logical canvas, then checks what the
 * container got. A stale rect from an earlier orientation is
 * force-corrected here, and ui_invalidate() runs so THIS frame repaints
 * with it, not next frame. */
int
ui_begin_screen(mu_Context* ctx_, const char* title, int opt) {
    const mu_Rect r = mu_rect(0, 0, ui_width(), ui_height());
    const int open = mu_begin_window_ex(ctx_, title, r, opt);

    if (open) {
        mu_Container* cnt = mu_get_current_container(ctx_);
        if (cnt->rect.x != r.x || cnt->rect.y != r.y || cnt->rect.w != r.w || cnt->rect.h != r.h) {
            cnt->rect = r;
            ui_invalidate();
        }
    }

    return open;
}
