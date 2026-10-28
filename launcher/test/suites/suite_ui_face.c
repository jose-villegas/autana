#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "draw/draw_font_roles.h"
#include "gfx/gfx.h"
#ifndef ESP_PLATFORM
#include "gfx/present/gfx_test.h"
#endif
#include "draw/draw_image_format.h"
#include "suites.h"
#include "testkit/cleanup.h"
#include "timing.h"
#include "ui/layout/ui_transform.h"
#include "ui/ui.h"
#include "ui/ui_internal.h"
#include "ui_probe.h"
#include "unity.h"

#define WINDOW_FLAGS      (MU_OPT_NOTITLE | MU_OPT_NORESIZE | MU_OPT_NOCLOSE | MU_OPT_NOFRAME)
#define FACE_RGB          0x314159
#define INK_RGB           0xABCDEF
#define SCOPE_SCALE       3
#define RANGE_LO          0
#define RANGE_HI          100
#define INITIAL_VALUE     25
#define TOUCH_HOLD_FRAMES 4
#define CHOICE_COUNT      3
#define SWATCH_COLS       2
#define SWATCH_ROWS       1
#define CENTRE_W          120
#define FADE_ALPHA        96
#define INNER_ALPHA       32
/* More rows than any canvas holds, so a scroll window overflows. */
#define OVERFLOW_ROWS     12
#define STRAY_SCROLL_X    7
static const recti_t BOX = {40, 100, 240, 60};
static const recti_t SECOND = {40, 240, 240, 60};
static const char* const NAMES[] = {"One", "Two", "Three"};
static const uint32_t SWATCHES[] = {FACE_RGB, INK_RGB};
static unsigned char* snapshot;
static size_t snapshot_size;
static int reports;
#ifndef ESP_PLATFORM
static bool scope_gfx;
#endif

/* The height text `label` was drawn at in the last built frame. */
static int
text_height_of(const char* label) {
    recti_t r;
    TEST_ASSERT_TRUE(ui_probe_text(label, &r));
    return r.h;
}

static void
cleanup(void) {
    free(snapshot);
    snapshot = NULL;
#ifndef ESP_PLATFORM
    ui_face_misuse_for_test = NULL;
    if (scope_gfx) {
        gfx_reset_for_test();
        scope_gfx = false;
    }
#endif
}

static void
fixture(void) {
    suite_set_test_cleanup(cleanup);
    ui_init();
    ui_set_transform(affine2x_identity());
    snapshot_size = 0;
    reports = 0;
}

static void
begin_legacy(void) {
    ui_begin(NULL);
    TEST_ASSERT_TRUE(ui_begin_screen(ui_context(), "Reference", WINDOW_FLAGS));
}

static void
end_legacy(void) {
    mu_end_window(ui_context());
    mu_end(ui_context());
    ui_end_pointer_frame();
}

static void
begin_face(void) {
    ui_begin(NULL);
    ui_screen();
}

static void
end_face(void) {
    ui_end_for_bands(UI_NO_BACKGROUND);
}

static void
save_commands(void) {
    snapshot = malloc(MU_COMMANDLIST_SIZE);
    TEST_ASSERT_NOT_NULL(snapshot);
    mu_Command* cmd = NULL;
    while (mu_next_command(ui_context(), &cmd)) {
        TEST_ASSERT_LESS_OR_EQUAL_UINT(MU_COMMANDLIST_SIZE, snapshot_size + cmd->base.size);
        memcpy(snapshot + snapshot_size, cmd, cmd->base.size);
        snapshot_size += cmd->base.size;
    }
    TEST_ASSERT_GREATER_THAN_UINT(0, snapshot_size);
}

static void
compare_commands(void) {
    size_t offset = 0;
    mu_Command* cmd = NULL;
    while (mu_next_command(ui_context(), &cmd)) {
        TEST_ASSERT_LESS_OR_EQUAL_UINT(snapshot_size, offset + cmd->base.size);
        TEST_ASSERT_EQUAL_MEMORY(snapshot + offset, cmd, cmd->base.size);
        offset += cmd->base.size;
    }
    TEST_ASSERT_EQUAL_UINT(snapshot_size, offset);
}

static const ui_theme_t THEME = {
    .panel_face = 0x202020,
    .panel_edge = 0x505050,
    .button_face = FACE_RGB,
    .accent = 0xE0B040,
    .accent_face = 0xB08020,
    .on_accent = 0x201000,
    .text = INK_RGB,
    .caption = 0x8090A0,
    .muted = 0x707070,
    .text_scale = 2,
    .icon_side = 32,
};

/* Spelled out rather than UI_THEME_FROM_FACE(), so the face's own
 * conversion is checked against an independent copy. */
#define LEGACY_THEME_COLOR(t, member) .member = ui_rgb((t).member)

static ui_mu_theme_t
legacy_theme(ui_theme_t t) {
    return (ui_mu_theme_t){LEGACY_THEME_COLOR(t, panel_face),
                           LEGACY_THEME_COLOR(t, panel_edge),
                           LEGACY_THEME_COLOR(t, button_face),
                           LEGACY_THEME_COLOR(t, accent),
                           LEGACY_THEME_COLOR(t, accent_face),
                           LEGACY_THEME_COLOR(t, on_accent),
                           LEGACY_THEME_COLOR(t, text),
                           LEGACY_THEME_COLOR(t, caption),
                           LEGACY_THEME_COLOR(t, muted),
                           .text_scale = t.text_scale,
                           .icon_side = t.icon_side};
}

static const uint8_t ICON_BITS[] = {0x80};
static const image_t ICON = {.pixels = (void*)ICON_BITS,
                             .format = IMAGE_MONO1,
                             .width = 1,
                             .height = 1,
                             .pitch = DRAW_IMAGE_MONO1_STRIDE_STEP / CHAR_BIT};
static const image_t* const ICONS[] = {&ICON, NULL, &ICON};

typedef enum {
    VERB_BUTTON,
    VERB_BUTTON_ICON,
    VERB_TILE,
    VERB_TILE_ICON,
    VERB_TOGGLE,
    VERB_SLIDER,
    VERB_DROPDOWN,
    VERB_DROPDOWN_ICONS,
    VERB_TEXT,
    VERB_TEXT_RIGHT,
    VERB_TEXT_CENTRE,
    VERB_CAPTION,
    VERB_CAPTION_CENTRE,
    VERB_HEADER,
    VERB_PANEL,
    VERB_FILL,
    VERB_OUTLINE,
    VERB_ICON,
    VERB_BEVEL,
    VERB_CHECK,
    VERB_SWATCHES
} verb_t;

static void
emit_legacy(verb_t verb) {
    mu_Context* ctx = ui_context();
    const mu_Rect r = ui_mu_from_recti(BOX);
    const ui_mu_theme_t t = legacy_theme(THEME);
    const ui_widget_button_t b = {.label = "Go", .enabled = true};
    const ui_widget_button_t with_icon = {.icon = &ICON, .label = "Go", .enabled = true};
    ui_span_t spans[UI_BEZEL_MAX_SPANS];
    int value = INITIAL_VALUE;
    int on = 0;
    const char* const items[] = {"One", "Two", "Three"};
    switch (verb) {
        case VERB_BUTTON: (void)ui_icon_button(ctx, "Go", r, &b, &t); break;
        case VERB_BUTTON_ICON: (void)ui_icon_button(ctx, "Go", r, &with_icon, &t); break;
        case VERB_TILE: (void)ui_tile_button(ctx, "Go", r, &b, &t); break;
        case VERB_TILE_ICON: (void)ui_tile_button(ctx, "Go", r, &with_icon, &t); break;
        case VERB_TOGGLE:
            mu_layout_set_next(ctx, r, 0);
            (void)mu_checkbox(ctx, "Go", &on);
            break;
        case VERB_SLIDER: (void)ui_theme_slider_int(ctx, r, &value, RANGE_LO, RANGE_HI, 1, &t); break;
        case VERB_DROPDOWN: (void)ui_mu_dropdown(ctx, "Go", r, items, NULL, CHOICE_COUNT, 0, &t); break;
        case VERB_DROPDOWN_ICONS: (void)ui_mu_dropdown(ctx, "Go", r, NAMES, ICONS, CHOICE_COUNT, 0, &t); break;
        case VERB_TEXT:
        case VERB_TEXT_RIGHT:
        case VERB_TEXT_CENTRE:
        case VERB_CAPTION:
        case VERB_CAPTION_CENTRE: {
            const bool centre = verb == VERB_TEXT_CENTRE || verb == VERB_CAPTION_CENTRE;
            const ui_align_t align = verb == VERB_TEXT_RIGHT ? UI_ALIGN_RIGHT
                                     : centre                ? UI_ALIGN_CENTRE
                                                             : UI_ALIGN_LEFT;
            const bool caption = verb == VERB_CAPTION || verb == VERB_CAPTION_CENTRE;
            ui_text_in(ctx, r, "Go", caption ? t.caption : t.text, t.text_scale, align);
            break;
        }
        case VERB_HEADER: ui_header_bar(ctx, r, "Go", t.text_scale, &t); break;
        case VERB_PANEL: ui_mu_panel(ctx, r, &t); break;
        case VERB_FILL: mu_draw_rect(ctx, r, ui_rgb(FACE_RGB)); break;
        case VERB_OUTLINE: mu_draw_box(ctx, r, ui_rgb(FACE_RGB)); break;
        case VERB_ICON: ui_draw_icon(ctx, r, &ICON, NULL, t.text); break;
        case VERB_BEVEL: {
            const int n = ui_bezel_spans(r, ui_rgb(FACE_RGB), true, spans, UI_BEZEL_MAX_SPANS);
            for (int i = 1; i < n; i++) {
                mu_draw_rect(ctx, spans[i].rect, spans[i].color);
            }
            break;
        }
        case VERB_CHECK: mu_draw_icon(ctx, MU_ICON_CHECK, r, t.text); break;
        case VERB_SWATCHES: ui_swatch_grid(ctx, r, SWATCHES, SWATCH_COLS, SWATCH_ROWS); break;
    }
}

static bool
emit_face(verb_t verb, int* value, bool* on) {
    switch (verb) {
        case VERB_BUTTON: return ui_button(BOX, "Go");
        case VERB_BUTTON_ICON: return ui_button_icon(BOX, "Go", &ICON);
        case VERB_TILE: return ui_tile(BOX, "Go", false);
        case VERB_TILE_ICON: return ui_tile_icon(BOX, "Go", &ICON, false);
        case VERB_TOGGLE: return ui_toggle(BOX, "Go", on);
        case VERB_SLIDER: return ui_slider(BOX, "Go", value, RANGE_LO, RANGE_HI);
        case VERB_DROPDOWN: return ui_dropdown(BOX, "Go", value, NAMES, CHOICE_COUNT);
        case VERB_DROPDOWN_ICONS: return ui_dropdown_icons(BOX, "Go", value, NAMES, ICONS, CHOICE_COUNT);
        case VERB_TEXT: ui_text(BOX, "Go"); break;
        case VERB_TEXT_RIGHT: ui_text_right(BOX, "Go"); break;
        case VERB_TEXT_CENTRE: ui_text_centre(BOX, "Go"); break;
        case VERB_CAPTION: ui_caption(BOX, "Go"); break;
        case VERB_CAPTION_CENTRE: ui_caption_centre(BOX, "Go"); break;
        case VERB_HEADER: ui_header(BOX, "Go"); break;
        case VERB_PANEL: ui_panel(BOX); break;
        case VERB_FILL: ui_fill(BOX, FACE_RGB); break;
        case VERB_OUTLINE: ui_outline(BOX, FACE_RGB); break;
        case VERB_ICON: ui_icon(BOX, &ICON, NULL); break;
        case VERB_BEVEL: ui_bevel(BOX, FACE_RGB, true); break;
        case VERB_CHECK: ui_check(BOX); break;
        case VERB_SWATCHES: ui_swatches(BOX, SWATCHES, SWATCH_COLS, SWATCH_ROWS); break;
    }
    return false;
}

static void
compare_verb(verb_t verb) {
    fixture();
    for (int i = 0; i < 2; i++) {
        begin_legacy();
        emit_legacy(verb);
        end_legacy();
    }
    save_commands();
    ui_init();
    int value = verb == VERB_DROPDOWN || verb == VERB_DROPDOWN_ICONS ? 0 : INITIAL_VALUE;
    bool on = false;
    for (int i = 0; i < 2; i++) {
        begin_face();
        if (verb != VERB_TOGGLE) {
            ui_theme(&THEME);
        }
        (void)emit_face(verb, &value, &on);
        end_face();
    }
    compare_commands();
}

static verb_t command_verb;

static void
test_verb_commands(void) {
    compare_verb(command_verb);
}

static void
assert_ink(const char* label, uint32_t rgb) {
    uint32_t drawn;
    TEST_ASSERT_TRUE(ui_probe_text_rgb(label, &drawn));
    TEST_ASSERT_EQUAL_HEX32(rgb, drawn);
    TEST_ASSERT_EQUAL_INT(UINT8_MAX, ui_probe_text_alpha(label));
}

static bool
has_face(uint32_t rgb) {
    const mu_Color color = ui_rgb(rgb);
    mu_Command* cmd = NULL;
    while (mu_next_command(ui_context(), &cmd)) {
        if (cmd->type == MU_COMMAND_RECT && memcmp(&cmd->rect.color, &color, sizeof color) == 0) {
            return true;
        }
    }
    return false;
}

static void
test_ui_screen_commands(void) {
    fixture();
    begin_legacy();
    mu_draw_rect(ui_context(), ui_mu_from_recti(BOX), ui_rgb(FACE_RGB));
    end_legacy();
    save_commands();
    ui_init();
    begin_face();
    ui_fill(BOX, FACE_RGB);
    end_face();
    compare_commands();
}

static void
test_ui_window_commands(void) {
    fixture();
    ui_begin(NULL);
    TEST_ASSERT_TRUE(mu_begin_window_ex(ui_context(), "Reference", ui_mu_from_recti(BOX), WINDOW_FLAGS));
    mu_draw_rect(ui_context(), ui_mu_from_recti(BOX), ui_rgb(FACE_RGB));
    end_legacy();
    save_commands();
    ui_init();
    ui_begin(NULL);
    ui_window(BOX);
    ui_fill(BOX, FACE_RGB);
    end_face();
    compare_commands();
}

static void
test_ui_row_and_next(void) {
    fixture();
    begin_legacy();
    const int widths[] = {-1};
    mu_layout_row(ui_context(), 1, widths, BOX.h);
    const mu_Rect expected = mu_layout_next(ui_context());
    end_legacy();
    ui_init();
    begin_face();
    ui_row(BOX.h);
    const recti_t actual = ui_next();
    end_face();
    TEST_ASSERT_EQUAL_INT(expected.x, actual.x);
    TEST_ASSERT_EQUAL_INT(expected.y, actual.y);
    TEST_ASSERT_EQUAL_INT(expected.w, actual.w);
    TEST_ASSERT_EQUAL_INT(expected.h, actual.h);
}

/* In one window: a centred row is the window-wide row narrowed to `w` about
 * its middle, a row wider than the window keeps the window's width, and
 * ui_row() after a centred row is window-wide again. */
static void
test_ui_row_centre_and_next(void) {
    fixture();
    begin_face();
    ui_row(BOX.h);
    const recti_t full = ui_next();
    ui_row_centre(CENTRE_W, BOX.h);
    const recti_t centred = ui_next();
    ui_row_centre(full.w + 1, BOX.h);
    const recti_t wide = ui_next();
    ui_row(BOX.h);
    const recti_t after = ui_next();
    end_face();
    TEST_ASSERT_EQUAL_INT(full.x + ((full.w - CENTRE_W) / 2), centred.x);
    TEST_ASSERT_GREATER_OR_EQUAL_INT(recti_bottom(full), centred.y);
    TEST_ASSERT_EQUAL_INT(CENTRE_W, centred.w);
    TEST_ASSERT_EQUAL_INT(BOX.h, centred.h);
    TEST_ASSERT_GREATER_OR_EQUAL_INT(recti_bottom(centred), wide.y);
    TEST_ASSERT_EQUAL_INT(full.x, wide.x);
    TEST_ASSERT_EQUAL_INT(full.w, wide.w);
    TEST_ASSERT_EQUAL_INT(full.x, after.x);
    TEST_ASSERT_EQUAL_INT(full.w, after.w);
}

/* A centred row ends with its window: the next window's first ui_next(),
 * with no ui_row() of its own, is as wide as one before any centring. */
static void
test_ui_row_centre_ends_with_window(void) {
    fixture();
    begin_face();
    const recti_t plain = ui_next();
    ui_screen();
    ui_row_centre(CENTRE_W, BOX.h);
    (void)ui_next();
    ui_screen();
    const recti_t after = ui_next();
    end_face();
    TEST_ASSERT_EQUAL_INT(plain.x, after.x);
    TEST_ASSERT_EQUAL_INT(plain.w, after.w);
}

static void
draw_overflowing_rows(void) {
    for (int i = 0; i < OVERFLOW_ROWS; i++) {
        ui_row(UI_ROW_HEIGHT);
        ui_fill(ui_next(), FACE_RGB);
    }
}

/* A face scroll window builds what a full-screen window with its horizontal
 * scroll locked builds. */
static void
test_ui_scroll_window_commands(void) {
    fixture();
    ui_begin(NULL);
    TEST_ASSERT_TRUE(ui_begin_screen(ui_context(), "Reference", WINDOW_FLAGS));
    mu_get_current_container(ui_context())->scroll.x = 0;
    for (int i = 0; i < OVERFLOW_ROWS; i++) {
        const int widths[] = {-1};
        mu_layout_row(ui_context(), 1, widths, UI_ROW_HEIGHT);
        mu_draw_rect(ui_context(), mu_layout_next(ui_context()), ui_rgb(FACE_RGB));
    }
    mu_end_window(ui_context());
    mu_end(ui_context());
    ui_end_pointer_frame();
    save_commands();
    ui_init();
    ui_begin(NULL);
    ui_scroll_window((recti_t){0, 0, ui_width(), ui_height()});
    draw_overflowing_rows();
    end_face();
    compare_commands();
}

/* Its rows overflow into content taller than the window, and it scrolls
 * vertically only: a horizontal scroll is undone on the next frame. */
static void
test_ui_scroll_window_scrolls_vertically(void) {
    fixture();
    const recti_t canvas = {0, 0, ui_width(), ui_height()};
    ui_begin(NULL);
    ui_scroll_window(canvas);
    draw_overflowing_rows();
    end_face();
    mu_Container* cnt = ui_context()->root_list.items[0];
    TEST_ASSERT_GREATER_THAN_INT(cnt->body.h, cnt->content_size.y);
    cnt->scroll.x = STRAY_SCROLL_X;
    ui_begin(NULL);
    ui_scroll_window(canvas);
    TEST_ASSERT_EQUAL_INT(0, cnt->scroll.x);
    draw_overflowing_rows();
    end_face();
}

typedef enum { ID_DUPLICATE, ID_WINDOWS, ID_HIDDEN, ID_DYNAMIC, ID_STABLE } identity_t;

static const char*
first_label(identity_t kind, bool changed) {
    if (kind == ID_HIDDEN) {
        return "Go##a";
    }
    if (kind == ID_DYNAMIC) {
        return changed ? "B###k" : "A###k";
    }
    return "Go";
}

/* The second button's label, or NULL for a kind with one button. */
static const char*
second_label(identity_t kind) {
    if (kind == ID_HIDDEN) {
        return "Go##b";
    }
    return kind == ID_DUPLICATE || kind == ID_WINDOWS ? "Go" : NULL;
}

static unsigned
identity_frame(identity_t kind, const input_t* in, bool changed) {
    ui_begin(in);
    ui_theme(&THEME);
    if (kind == ID_WINDOWS) {
        ui_window((recti_t){0, 0, ui_width(), SECOND.y});
    } else {
        ui_screen();
    }
    unsigned hits = ui_button(BOX, first_label(kind, changed)) ? 1U : 0U;
    if (kind == ID_WINDOWS) {
        ui_window((recti_t){0, SECOND.y, ui_width(), ui_height() - SECOND.y});
    }
    const char* second = second_label(kind);
    if (second != NULL && ui_button(SECOND, second)) {
        hits |= 2U;
    }
    end_face();
    return hits;
}

static input_t
touch_sample(recti_t r, bool down, bool edge) {
    const int x = r.x + r.w / 2, y = r.y + r.h / 2;
    return (input_t){
        .pressed = down && edge, .released = !down && edge, .down = down, .point = {x, y}, .press = {x, y}};
}

static unsigned
contact_identity(identity_t kind, recti_t target) {
    const input_t idle = {0};
    const input_t press = touch_sample(target, true, true);
    const input_t release = touch_sample(target, false, true);
    (void)identity_frame(kind, &idle, false);
    (void)identity_frame(kind, &idle, false);
    unsigned hits = identity_frame(kind, &press, false);
    hits |= identity_frame(kind, &release, true);
    hits |= identity_frame(kind, &idle, true);
    return hits;
}

static void
test_identity_duplicate_label(void) {
    fixture();
    TEST_ASSERT_EQUAL_UINT(2, contact_identity(ID_DUPLICATE, SECOND));
}

static void
test_identity_two_windows(void) {
    fixture();
    TEST_ASSERT_EQUAL_UINT(2, contact_identity(ID_WINDOWS, SECOND));
}

static void
test_identity_hidden_labels(void) {
    fixture();
    const input_t idle = {0};
    (void)identity_frame(ID_HIDDEN, &idle, false);
    (void)identity_frame(ID_HIDDEN, &idle, false);
    int count = 0;
    mu_Command* cmd = NULL;
    while (mu_next_command(ui_context(), &cmd)) {
        if (cmd->type == MU_COMMAND_TEXT) {
            TEST_ASSERT_EQUAL_STRING("Go", cmd->text.str);
            count++;
        }
    }
    TEST_ASSERT_EQUAL_INT(2, count);
    TEST_ASSERT_EQUAL_UINT(2, contact_identity(ID_HIDDEN, SECOND));
}

static void
assert_release_identity(identity_t kind) {
    const input_t idle = {0};
    const input_t press = touch_sample(BOX, true, true);
    const input_t release = touch_sample(BOX, false, true);
    (void)identity_frame(kind, &idle, false);
    (void)identity_frame(kind, &idle, false);
    TEST_ASSERT_EQUAL_UINT(0, identity_frame(kind, &press, false));
    TEST_ASSERT_EQUAL_UINT(1, identity_frame(kind, &release, true));
    TEST_ASSERT_EQUAL_UINT(0, identity_frame(kind, &idle, true));
}

static void
test_identity_dynamic_label(void) {
    fixture();
    assert_release_identity(ID_DYNAMIC);
}

static void
test_identity_across_frames(void) {
    fixture();
    assert_release_identity(ID_STABLE);
}

static bool
widget_frame(verb_t verb, const input_t* in, int* value, bool* on, bool disabled, bool nested) {
    ui_begin(in);
    ui_screen();
    ui_theme(&THEME);
    ui_disabled_begin(disabled);
    if (nested) {
        ui_disabled_begin(false);
    }
    const bool hit = emit_face(verb, value, on);
    if (nested) {
        ui_disabled_end();
    }
    ui_disabled_end();
    end_face();
    return hit;
}

static unsigned
tap_widget(verb_t verb, recti_t target, int* value, bool* on, bool disabled, bool nested) {
    const input_t idle = {0};
    const input_t press = touch_sample(target, true, true);
    const input_t hold = touch_sample(target, true, false);
    const input_t release = touch_sample(target, false, true);
    (void)widget_frame(verb, &idle, value, on, disabled, nested);
    (void)widget_frame(verb, &idle, value, on, disabled, nested);
    unsigned hit = widget_frame(verb, &press, value, on, disabled, nested);
    for (int i = 0; i < TOUCH_HOLD_FRAMES; i++) {
        hit += widget_frame(verb, &hold, value, on, disabled, nested);
    }
    hit += widget_frame(verb, &release, value, on, disabled, nested);
    return hit;
}

static void
widget_hit(verb_t verb) {
    fixture();
    int value = INITIAL_VALUE;
    bool on = false;
    TEST_ASSERT_TRUE(tap_widget(verb, BOX, &value, &on, false, false));
}

static void
test_ui_button_hit(void) {
    widget_hit(VERB_BUTTON);
}

static void
test_ui_tile_hit(void) {
    widget_hit(VERB_TILE);
}

static void
test_ui_icon_widgets_hit(void) {
    widget_hit(VERB_BUTTON_ICON);
    widget_hit(VERB_TILE_ICON);
}

static void
test_ui_tile_icon_rect_is_the_tiles_own(void) {
    fixture();
    begin_face();
    ui_theme(&THEME);
    const recti_t face = ui_tile_icon_rect(BOX);
    end_face();
    const ui_mu_theme_t t = legacy_theme(THEME);
    const mu_Rect legacy = ui_mu_tile_icon_rect(ui_mu_from_recti(BOX), &t);
    TEST_ASSERT_EQUAL_INT(legacy.x, face.x);
    TEST_ASSERT_EQUAL_INT(legacy.y, face.y);
    TEST_ASSERT_EQUAL_INT(legacy.w, face.w);
    TEST_ASSERT_EQUAL_INT(legacy.h, face.h);
}

/* A label wider than its slot shows up to the control's own edges, as
 * mu_button() clipped: the whole button, or the part right of an icon. A
 * dropdown's label still stops short of its chevron. */
#define OVERFLOW_LABEL "A label far wider than any button"

static recti_t
label_clip(verb_t verb) {
    fixture();
    int value = 0;
    begin_face();
    ui_theme(&THEME);
    switch (verb) {
        case VERB_BUTTON: (void)ui_button(BOX, OVERFLOW_LABEL); break;
        case VERB_BUTTON_ICON: (void)ui_button_icon(BOX, OVERFLOW_LABEL, &ICON); break;
        case VERB_TILE: (void)ui_tile(BOX, OVERFLOW_LABEL, false); break;
        default: {
            const char* const names[] = {OVERFLOW_LABEL};
            (void)ui_dropdown(BOX, "Go", &value, names, 1);
            break;
        }
    }
    end_face();
    recti_t clip = {0};
    TEST_ASSERT_TRUE(ui_probe_text_clip(OVERFLOW_LABEL, &clip));
    return clip;
}

static void
test_overflowing_labels_clip_at_the_control_edges(void) {
    const recti_t button = label_clip(VERB_BUTTON);
    TEST_ASSERT_EQUAL_INT(BOX.x, button.x);
    TEST_ASSERT_EQUAL_INT(BOX.w, button.w);

    const recti_t tile = label_clip(VERB_TILE);
    TEST_ASSERT_EQUAL_INT(BOX.x, tile.x);
    TEST_ASSERT_EQUAL_INT(BOX.w, tile.w);

    /* The label slot's left edge is the button's right edge less its pad
     * and the slot's own width; the padding is read back from the label
     * widths rather than restated here. */
    const recti_t icon = label_clip(VERB_BUTTON_ICON);
    const int pad = (BOX.w - ui_probe_button_label_width(BOX.w, false, &THEME)) / 2;
    const int slot_x = recti_right(BOX) - pad - ui_probe_button_label_width(BOX.w, true, &THEME);
    TEST_ASSERT_GREATER_THAN_INT(BOX.x + THEME.icon_side, slot_x);
    TEST_ASSERT_EQUAL_INT_MESSAGE(slot_x, icon.x, "an icon button's label clips at its slot, not into the icon");
    TEST_ASSERT_EQUAL_INT(recti_right(BOX), recti_right(icon));

    const recti_t dropdown = label_clip(VERB_DROPDOWN);
    TEST_ASSERT_LESS_THAN_INT(recti_right(BOX), recti_right(dropdown));
}

/* A raised bevel is the legacy emitter's raised spans, and span 0 - the
 * face - is left to the caller: a face drawn first stays the only rect at
 * the full rectangle. */
static int
full_rects_at_box(void) {
    int count = 0;
    mu_Command* cmd = NULL;
    const mu_Rect box = ui_mu_from_recti(BOX);
    while (mu_next_command(ui_context(), &cmd)) {
        const mu_Rect c = cmd->rect.rect;
        count += cmd->type == MU_COMMAND_RECT && c.x == box.x && c.y == box.y && c.w == box.w && c.h == box.h;
    }
    return count;
}

static void
test_ui_bevel_raised_matches_legacy_and_leaves_the_face(void) {
    fixture();
    for (int i = 0; i < 2; i++) {
        begin_legacy();
        mu_draw_rect(ui_context(), ui_mu_from_recti(BOX), ui_rgb(FACE_RGB));
        ui_span_t spans[UI_BEZEL_MAX_SPANS];
        const int n = ui_bezel_spans(ui_mu_from_recti(BOX), ui_rgb(FACE_RGB), false, spans, UI_BEZEL_MAX_SPANS);
        TEST_ASSERT_GREATER_THAN_INT(1, n);
        for (int span = 1; span < n; span++) {
            mu_draw_rect(ui_context(), spans[span].rect, spans[span].color);
        }
        end_legacy();
    }
    save_commands();
    ui_init();
    for (int i = 0; i < 2; i++) {
        begin_face();
        ui_fill(BOX, FACE_RGB);
        ui_bevel(BOX, FACE_RGB, false);
        end_face();
    }
    compare_commands();
    TEST_ASSERT_EQUAL_INT_MESSAGE(1, full_rects_at_box(), "the bevel must not redraw the face");
}

static void
test_ui_toggle_hit(void) {
    fixture();
    int value = INITIAL_VALUE;
    bool on = false;
    TEST_ASSERT_TRUE(tap_widget(VERB_TOGGLE, BOX, &value, &on, false, false));
    TEST_ASSERT_TRUE(on);
}

static void
test_ui_slider_drag(void) {
    fixture();
    int value = INITIAL_VALUE;
    bool on = false;
    const input_t idle = {0};
    const int y = BOX.y + BOX.h / 2;
    const input_t press = {.down = true, .pressed = true, .point = {BOX.x + BOX.w / 4, y}};
    const input_t drag = {.down = true, .point = {BOX.x + BOX.w * 3 / 4, y}};
    (void)widget_frame(VERB_SLIDER, &idle, &value, &on, false, false);
    (void)widget_frame(VERB_SLIDER, &idle, &value, &on, false, false);
    (void)widget_frame(VERB_SLIDER, &press, &value, &on, false, false);
    bool changed = false;
    for (int i = 0; i < TOUCH_HOLD_FRAMES; i++) {
        changed |= widget_frame(VERB_SLIDER, &drag, &value, &on, false, false);
    }
    TEST_ASSERT_TRUE(changed);
    TEST_ASSERT_GREATER_THAN_INT(INITIAL_VALUE, value);
}

static void
dropdown_pick(verb_t verb) {
    fixture();
    int value = 0;
    bool on = false;
    (void)tap_widget(verb, BOX, &value, &on, false, false);
    const int row_h = BOX.h;
    const mu_Rect list = ui_dropdown_list_rect(ui_mu_from_recti(BOX), CHOICE_COUNT, row_h, ui_height(), UI_MARGIN);
    const int x = list.x + list.w / 2, y = list.y + row_h + row_h / 2;
    const input_t idle = {0}, press = {.down = true, .pressed = true, .point = {x, y}},
                  release = {.released = true, .point = {x, y}};
    (void)widget_frame(verb, &idle, &value, &on, false, false);
    bool hit = widget_frame(verb, &press, &value, &on, false, false);
    hit |= widget_frame(verb, &release, &value, &on, false, false);
    TEST_ASSERT_TRUE(hit);
    TEST_ASSERT_EQUAL_INT(1, value);
}

static void
test_ui_dropdown_pick(void) {
    dropdown_pick(VERB_DROPDOWN);
}

static void
test_ui_dropdown_icons_pick(void) {
    dropdown_pick(VERB_DROPDOWN_ICONS);
}

static void
test_disabled_true_is_muted(void) {
    fixture();
    int value = 0;
    bool on = false;
    TEST_ASSERT_FALSE(tap_widget(VERB_BUTTON, BOX, &value, &on, true, false));
    assert_ink("Go", THEME.muted);
}

static void
test_disabled_false_is_unchanged(void) {
    fixture();
    int value = 0;
    bool on = false;
    TEST_ASSERT_TRUE(tap_widget(VERB_BUTTON, BOX, &value, &on, false, false));
    assert_ink("Go", THEME.text);
}

static void
test_disabled_nested_false_stays_disabled(void) {
    fixture();
    int value = 0;
    bool on = false;
    TEST_ASSERT_FALSE(tap_widget(VERB_BUTTON, BOX, &value, &on, true, true));
    assert_ink("Go", THEME.muted);
}

static void
test_color_face_scope(void) {
    fixture();
    begin_face();
    ui_theme(&THEME);
    ui_color_begin(UI_COLOR_FACE, INK_RGB);
    (void)ui_button(BOX, "Scoped");
    ui_color_end();
    (void)ui_button(SECOND, "Rest");
    end_face();
    TEST_ASSERT_TRUE(has_face(INK_RGB));
    TEST_ASSERT_TRUE(has_face(THEME.button_face));
}

static void
test_color_text_scope(void) {
    fixture();
    begin_face();
    ui_theme(&THEME);
    ui_color_begin(UI_COLOR_TEXT, FACE_RGB);
    ui_text(BOX, "Text");
    ui_text_right(BOX, "Right");
    ui_text_centre(BOX, "Centre");
    ui_caption(BOX, "Caption");
    (void)ui_button(SECOND, "Label");
    ui_color_end();
    ui_text(BOX, "Rest");
    end_face();
    assert_ink("Text", FACE_RGB);
    assert_ink("Right", FACE_RGB);
    assert_ink("Centre", FACE_RGB);
    assert_ink("Caption", FACE_RGB);
    assert_ink("Label", FACE_RGB);
    assert_ink("Rest", THEME.text);
}

/* A flow row sized with ui_text_height() fits the text the same scale draws. */
static void
test_text_height_follows_scale(void) {
    fixture();
    begin_face();
    ui_theme(&THEME);
    const int theme_height = ui_text_height();
    ui_scale_begin(SCOPE_SCALE);
    const int scoped_height = ui_text_height();
    ui_text(BOX, "Text");
    ui_scale_end();
    end_face();
    TEST_ASSERT_EQUAL_INT(text_height_of("Text"), scoped_height);
    TEST_ASSERT_EQUAL_INT(draw_font_height(draw_font_ui(), THEME.text_scale), theme_height);
}

static void
test_scale_scope(void) {
    fixture();
    begin_face();
    ui_theme(&THEME);
    ui_scale_begin(SCOPE_SCALE);
    ui_text(BOX, "Text");
    (void)ui_button(SECOND, "Label");
    ui_scale_end();
    ui_text(BOX, "Rest");
    end_face();
    const int height = text_height_of("Text");
    TEST_ASSERT_EQUAL_INT(height, text_height_of("Label"));
    TEST_ASSERT_EQUAL_INT(draw_font_height(draw_font_ui(), SCOPE_SCALE), height);
    TEST_ASSERT_EQUAL_INT(draw_font_height(draw_font_ui(), THEME.text_scale), text_height_of("Rest"));
}

static void
assert_alpha(const char* label, unsigned char alpha) {
    TEST_ASSERT_EQUAL_INT(alpha, ui_probe_text_alpha(label));
}

/* Every text verb inside the scope takes its alpha, the innermost scope
 * wins, and text after it is opaque again. */
static void
test_alpha_scope(void) {
    fixture();
    begin_face();
    ui_theme(&THEME);
    ui_alpha_begin(FADE_ALPHA);
    ui_text(BOX, "Text");
    ui_text_right(BOX, "Right");
    ui_text_centre(BOX, "Centre");
    ui_caption(BOX, "Caption");
    ui_alpha_begin(INNER_ALPHA);
    ui_text(BOX, "Inner");
    ui_alpha_end();
    ui_text(BOX, "Outer");
    ui_alpha_end();
    ui_text(BOX, "Rest");
    end_face();
    assert_alpha("Text", FADE_ALPHA);
    assert_alpha("Right", FADE_ALPHA);
    assert_alpha("Centre", FADE_ALPHA);
    assert_alpha("Caption", FADE_ALPHA);
    assert_alpha("Inner", INNER_ALPHA);
    assert_alpha("Outer", FADE_ALPHA);
    assert_alpha("Rest", UINT8_MAX);
    assert_ink("Rest", THEME.text);
}

/* The scope scales an ink's own coverage: a caption the shell look leaves
 * clear stays clear inside it. */
static void
test_alpha_scales_ink_coverage(void) {
    fixture();
    begin_face();
    ui_alpha_begin(FADE_ALPHA);
    ui_caption(BOX, "Clear");
    ui_alpha_end();
    end_face();
    TEST_ASSERT_EQUAL_INT(ui_theme_from_style(ui_context()->style).caption.a * FADE_ALPHA / UINT8_MAX,
                          ui_probe_text_alpha("Clear"));
}

#ifndef ESP_PLATFORM
static void
report_imbalance(void) {
    reports++;
}

static void
scope_fixture(void) {
    fixture();
    TEST_ASSERT_TRUE(gfx_init());
    scope_gfx = true;
    ui_face_misuse_for_test = report_imbalance;
}

static void
assert_scale(const char* label, int scale) {
    TEST_ASSERT_EQUAL_INT(draw_font_height(draw_font_ui(), scale), text_height_of(label));
}

static void
test_scope_depth_boundary(void) {
    scope_fixture();
    for (int depth = UI_FACE_SCOPE_DEPTH; depth <= UI_FACE_SCOPE_DEPTH + 1; depth++) {
        reports = 0;
        begin_face();
        ui_theme(&THEME);
        for (int i = 0; i < depth; i++) {
            ui_color_begin(UI_COLOR_TEXT, FACE_RGB);
            ui_scale_begin(SCOPE_SCALE);
            ui_disabled_begin(true);
            ui_alpha_begin(FADE_ALPHA);
        }
        for (int i = 0; i < depth; i++) {
            ui_alpha_end();
            ui_disabled_end();
            ui_scale_end();
            ui_color_end();
        }
        ui_text(BOX, "Restored");
        end_face();
        TEST_ASSERT_EQUAL_INT(depth > UI_FACE_SCOPE_DEPTH ? 1 : 0, reports);
        assert_ink("Restored", THEME.text);
        assert_scale("Restored", THEME.text_scale);
        assert_alpha("Restored", UINT8_MAX);
    }
}

static void
test_nested_scope_values(void) {
    fixture();
    begin_face();
    ui_theme(&THEME);
    ui_color_begin(UI_COLOR_TEXT, FACE_RGB);
    ui_scale_begin(SCOPE_SCALE);
    ui_color_begin(UI_COLOR_TEXT, THEME.caption);
    ui_scale_begin(SCOPE_SCALE + 1);
    ui_text(BOX, "Inner");
    ui_scale_end();
    ui_color_end();
    ui_text(BOX, "Outer");
    ui_scale_end();
    ui_color_end();
    ui_text(BOX, "After");
    end_face();
    assert_ink("Inner", THEME.caption);
    assert_scale("Inner", SCOPE_SCALE + 1);
    assert_ink("Outer", FACE_RGB);
    assert_scale("Outer", SCOPE_SCALE);
    assert_ink("After", THEME.text);
    assert_scale("After", THEME.text_scale);
}

static void
test_disabled_every_widget(void) {
    static const verb_t verbs[] = {VERB_TILE, VERB_TOGGLE, VERB_SLIDER, VERB_DROPDOWN, VERB_DROPDOWN_ICONS};
    for (size_t i = 0; i < sizeof verbs / sizeof verbs[0]; i++) {
        const verb_t verb = verbs[i];
        const bool dropdown = verb == VERB_DROPDOWN || verb == VERB_DROPDOWN_ICONS;
        for (int disabled = 0; disabled <= 1; disabled++) {
            fixture();
            int value = dropdown ? 0 : INITIAL_VALUE;
            bool on = false;
            unsigned hits = tap_widget(verb, BOX, &value, &on, disabled != 0, false);
            if (dropdown) {
                const mu_Rect list =
                    ui_dropdown_list_rect(ui_mu_from_recti(BOX), CHOICE_COUNT, BOX.h, ui_height(), UI_MARGIN);
                const recti_t row = {list.x, list.y + BOX.h, list.w, BOX.h};
                hits += tap_widget(verb, row, &value, &on, disabled != 0, false);
            }
            if (disabled) {
                TEST_ASSERT_EQUAL_UINT(0, hits);
                TEST_ASSERT_EQUAL_INT(dropdown ? 0 : INITIAL_VALUE, value);
                TEST_ASSERT_FALSE(on);
            } else {
                TEST_ASSERT_EQUAL_UINT(1, hits);
            }
        }
    }
}

static void
tap_release_contract(verb_t verb) {
    fixture();
    int value = INITIAL_VALUE;
    bool on = false;
    const input_t idle = {0};
    const input_t press = touch_sample(BOX, true, true);
    const input_t hold = touch_sample(BOX, true, false);
    const input_t release = touch_sample(BOX, false, true);
    (void)widget_frame(verb, &idle, &value, &on, false, false);
    (void)widget_frame(verb, &idle, &value, &on, false, false);
    TEST_ASSERT_FALSE(widget_frame(verb, &press, &value, &on, false, false));
    for (int i = 0; i < TOUCH_HOLD_FRAMES; i++) {
        TEST_ASSERT_FALSE(widget_frame(verb, &hold, &value, &on, false, false));
        TEST_ASSERT_FALSE(on);
    }
    TEST_ASSERT_TRUE(widget_frame(verb, &release, &value, &on, false, false));
    TEST_ASSERT_EQUAL_INT(verb == VERB_TOGGLE, on);
    TEST_ASSERT_FALSE(widget_frame(verb, &idle, &value, &on, false, false));
    TEST_ASSERT_EQUAL_UINT(1, tap_widget(verb, BOX, &value, &on, false, false));
    TEST_ASSERT_FALSE(on);
    const recti_t outside = {0, 0, 1, 1};
    TEST_ASSERT_EQUAL_UINT(0, tap_widget(verb, outside, &value, &on, false, false));
    (void)widget_frame(verb, &press, &value, &on, false, false);
    (void)widget_frame(verb, &hold, &value, &on, false, false);
    const input_t out_release = touch_sample(outside, false, true);
    TEST_ASSERT_FALSE(widget_frame(verb, &out_release, &value, &on, false, false));
    TEST_ASSERT_FALSE(on);
}

static void
test_tap_release_contract(void) {
    tap_release_contract(VERB_BUTTON);
    tap_release_contract(VERB_BUTTON_ICON);
    tap_release_contract(VERB_TILE);
    tap_release_contract(VERB_TILE_ICON);
    tap_release_contract(VERB_TOGGLE);
}

/* Either side of the default snap.reach (40, ui_build.c). */
#define SNAP_NEAR 20
#define SNAP_FAR  80

/* A finger's centre a given distance left of BOX, level with its middle. */
static recti_t
left_of_box(int gap) {
    return (recti_t){BOX.x - gap, BOX.y + BOX.h / 2, 0, 0};
}

static void
snapped_tap_contract(verb_t verb) {
    fixture();
    int value = INITIAL_VALUE;
    bool on = false;
    TEST_ASSERT_EQUAL_UINT_MESSAGE(1, tap_widget(verb, left_of_box(SNAP_NEAR), &value, &on, false, false),
                                   "a press and release just outside a control fire it");
    fixture();
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0, tap_widget(verb, left_of_box(SNAP_FAR), &value, &on, false, false),
                                   "a tap farther than snap.reach fires nothing");
}

static void
test_a_tap_just_outside_fires_the_nearest_control(void) {
    snapped_tap_contract(VERB_BUTTON);
    snapped_tap_contract(VERB_BUTTON_ICON);
    snapped_tap_contract(VERB_TILE);
    snapped_tap_contract(VERB_TILE_ICON);
    snapped_tap_contract(VERB_TOGGLE);
}

static void
test_a_press_that_slides_off_cancels_only_beyond_the_reach(void) {
    fixture();
    int value = INITIAL_VALUE;
    bool on = false;
    const input_t idle = {0};
    const recti_t lifts[] = {left_of_box(SNAP_NEAR), left_of_box(SNAP_FAR)};
    for (size_t i = 0; i < 2; i++) {
        fixture();
        const input_t press = touch_sample(BOX, true, true);
        const input_t release = touch_sample(lifts[i], false, true);
        (void)widget_frame(VERB_BUTTON, &idle, &value, &on, false, false);
        (void)widget_frame(VERB_BUTTON, &idle, &value, &on, false, false);
        (void)widget_frame(VERB_BUTTON, &press, &value, &on, false, false);
        TEST_ASSERT_EQUAL_INT_MESSAGE(i == 0, widget_frame(VERB_BUTTON, &release, &value, &on, false, false),
                                      "a lift within reach fires, one beyond it cancels");
    }
}

static void
test_unchanged_picks_return_false(void) {
    fixture();
    int value = 0;
    bool on = false;
    TEST_ASSERT_EQUAL_UINT(0, tap_widget(VERB_DROPDOWN, BOX, &value, &on, false, false));
    const mu_Rect list = ui_dropdown_list_rect(ui_mu_from_recti(BOX), CHOICE_COUNT, BOX.h, ui_height(), UI_MARGIN);
    const recti_t current = {list.x, list.y, list.w, BOX.h};
    TEST_ASSERT_EQUAL_UINT(0, tap_widget(VERB_DROPDOWN, current, &value, &on, false, false));
    TEST_ASSERT_EQUAL_INT(0, value);
    ui_init();
    value = (RANGE_LO + RANGE_HI) / 2;
    TEST_ASSERT_EQUAL_UINT(0, tap_widget(VERB_SLIDER, BOX, &value, &on, false, false));
    TEST_ASSERT_EQUAL_INT((RANGE_LO + RANGE_HI) / 2, value);
}

static void
pair_frame(verb_t verb, const input_t* in, bool* first, bool* second) {
    ui_begin(in);
    ui_screen();
    if (verb == VERB_TOGGLE) {
        (void)ui_toggle(BOX, "Same", first);
        (void)ui_toggle(SECOND, "Same", second);
    } else {
        int index = 0;
        (void)ui_dropdown(BOX, "Same", &index, NAMES, CHOICE_COUNT);
        (void)ui_dropdown(SECOND, "Same", &index, NAMES, CHOICE_COUNT);
    }
    end_face();
}

static void
tap_pair(verb_t verb, recti_t target, bool* first, bool* second) {
    const input_t idle = {0};
    const input_t press = touch_sample(target, true, true);
    const input_t release = touch_sample(target, false, true);
    pair_frame(verb, &idle, first, second);
    pair_frame(verb, &idle, first, second);
    pair_frame(verb, &press, first, second);
    pair_frame(verb, &release, first, second);
}

static void
test_duplicate_toggle_identity(void) {
    fixture();
    bool first = false, second = false;
    tap_pair(VERB_TOGGLE, SECOND, &first, &second);
    TEST_ASSERT_FALSE(first);
    TEST_ASSERT_TRUE(second);
    tap_pair(VERB_TOGGLE, BOX, &first, &second);
    TEST_ASSERT_TRUE(first);
    TEST_ASSERT_TRUE(second);
}

static mu_Id
opened_pair_id(recti_t target) {
    ui_init();
    bool first = false, second = false;
    tap_pair(VERB_DROPDOWN, target, &first, &second);
    mu_Context* ctx = ui_context();
    TEST_ASSERT_EQUAL_INT(2, ctx->root_list.idx);
    const mu_Container* popup = ctx->root_list.items[ctx->root_list.idx - 1];
    return ctx->container_pool[popup - ctx->containers].id;
}

static void
test_duplicate_dropdown_identity(void) {
    fixture();
    const mu_Id first = opened_pair_id(BOX);
    const mu_Id second = opened_pair_id(SECOND);
    TEST_ASSERT_NOT_EQUAL(first, second);
}

static void
test_empty_visible_labels(void) {
    fixture();
    begin_face();
    bool on = true;
    (void)ui_button(BOX, "##x");
    (void)ui_toggle(SECOND, "###x", &on);
    end_face();
    mu_Command* cmd = NULL;
    while (mu_next_command(ui_context(), &cmd)) {
        TEST_ASSERT_NOT_EQUAL(MU_COMMAND_TEXT, cmd->type);
    }
}

static void
test_frame_state_reset(void) {
    scope_fixture();
    const ui_mu_theme_t base = ui_theme_from_style(ui_context()->style);
    for (int state = 0; state < CHOICE_COUNT; state++) {
        begin_face();
        if (state == 0) {
            ui_theme(&THEME);
        } else if (state == 1) {
            ui_color_begin(UI_COLOR_TEXT, FACE_RGB);
            ui_scale_begin(SCOPE_SCALE);
        } else {
            ui_disabled_begin(true);
        }
        end_face();
        bool on = false;
        const input_t idle = {0};
        const input_t press = touch_sample(BOX, true, true);
        const input_t release = touch_sample(BOX, false, true);
        const input_t frames[] = {idle, idle, press, release};
        unsigned hits = 0;
        for (size_t i = 0; i < sizeof frames / sizeof frames[0]; i++) {
            ui_begin(&frames[i]);
            ui_screen();
            hits += ui_toggle(BOX, "Reset", &on);
            end_face();
        }
        TEST_ASSERT_EQUAL_UINT(1, hits);
        uint32_t drawn;
        TEST_ASSERT_TRUE(ui_probe_text_rgb("Reset", &drawn));
        TEST_ASSERT_EQUAL_HEX32(ui_rgb_value(base.text), drawn);
        TEST_ASSERT_EQUAL_INT(base.text.a, ui_probe_text_alpha("Reset"));
        assert_scale("Reset", base.text_scale);
    }
}

static void
backdrop(void) {}

static void
test_all_end_paths(void) {
    scope_fixture();
    for (int over = 0; over <= 1; over++) {
        for (int open = 0; open <= 1; open++) {
            reports = 0;
            begin_face();
            ui_color_begin(UI_COLOR_TEXT, FACE_RGB);
            if (!open) {
                ui_color_end();
            }
            if (over) {
                (void)ui_end_over(backdrop);
            } else {
                ui_end_for_bands(UI_NO_BACKGROUND);
            }
            TEST_ASSERT_EQUAL_INT(open, reports);
            TEST_ASSERT_EQUAL_INT(0, ui_context()->container_stack.idx);
            begin_face();
            end_face();
            TEST_ASSERT_EQUAL_INT(open, reports);
        }
    }
}

static void
test_window_order_contract(void) {
    scope_fixture();
    ui_begin(NULL);
    ui_window(BOX);
    ui_screen();
    end_face();
    TEST_ASSERT_EQUAL_INT(0, ui_context()->container_stack.idx);
    ui_begin(NULL);
    (void)ui_end(UI_NO_BACKGROUND);
    TEST_ASSERT_EQUAL_INT(0, ui_context()->container_stack.idx);
    ui_begin(NULL);
    ui_row(BOX.h);
    const recti_t next = ui_next();
    const recti_t empty = {0};
    TEST_ASSERT_EQUAL_MEMORY(&empty, &next, sizeof empty);
    end_face();
    TEST_ASSERT_EQUAL_INT(0, ui_context()->container_stack.idx);
}

typedef enum { STATE_TILE, STATE_TOGGLE, STATE_DROPDOWN, STATE_HIDDEN, STATE_DISABLED } command_state_t;

static void
state_commands(command_state_t state, bool face) {
    const ui_mu_theme_t t = legacy_theme(THEME);
    const ui_widget_button_t button = {
        .label = "Go", .enabled = state != STATE_DISABLED, .selected = state == STATE_TILE};
    const mu_Rect r = ui_mu_from_recti(BOX);
    int index = CHOICE_COUNT - 1;
    bool on = true;
    int legacy_on = on;
    if (face) {
        if (state != STATE_TOGGLE) {
            ui_theme(&THEME);
        }
        ui_disabled_begin(state == STATE_DISABLED);
        switch (state) {
            case STATE_TILE: (void)ui_tile(BOX, "Go", true); break;
            case STATE_TOGGLE: (void)ui_toggle(BOX, "Go", &on); break;
            case STATE_DROPDOWN: (void)ui_dropdown(BOX, "Go", &index, NAMES, CHOICE_COUNT); break;
            case STATE_HIDDEN: (void)ui_button(BOX, "Go##id"); break;
            case STATE_DISABLED: (void)ui_button(BOX, "Go"); break;
        }
        ui_disabled_end();
    } else {
        switch (state) {
            case STATE_TILE: (void)ui_tile_button(ui_context(), "Go", r, &button, &t); break;
            case STATE_TOGGLE:
                mu_layout_set_next(ui_context(), r, 0);
                (void)mu_checkbox(ui_context(), "Go", &legacy_on);
                break;
            case STATE_DROPDOWN:
                (void)ui_mu_dropdown(ui_context(), "Go", r, NAMES, NULL, CHOICE_COUNT, index, &t);
                break;
            case STATE_HIDDEN:
            case STATE_DISABLED: (void)ui_icon_button(ui_context(), "Go", r, &button, &t); break;
        }
    }
}

static void
test_additional_command_states(void) {
    for (command_state_t state = STATE_TILE; state <= STATE_DISABLED; state++) {
        fixture();
        begin_legacy();
        state_commands(state, false);
        end_legacy();
        save_commands();
        ui_init();
        begin_face();
        state_commands(state, true);
        end_face();
        compare_commands();
        cleanup();
    }
    fixture();
    begin_face();
    ui_swatches(BOX, SWATCHES, 0, SWATCH_ROWS);
    ui_swatches(BOX, SWATCHES, -1, SWATCH_ROWS);
    end_face();
    mu_Command* cmd = NULL;
    TEST_ASSERT_FALSE(mu_next_command(ui_context(), &cmd));
}

static void
test_identity_capacity_reports(void) {
    scope_fixture();
    begin_face();
    ui_context()->on_control = NULL;
    char label[sizeof "id " + sizeof(int) * 3];
    for (int i = 0; i < UI_FACE_IDS_MAX; i++) {
        snprintf(label, sizeof label, "id %d", i);
        (void)ui_button(BOX, label);
    }
    TEST_ASSERT_EQUAL_INT(0, reports);
    (void)ui_button(BOX, "overflow");
    end_face();
    TEST_ASSERT_EQUAL_INT(1, reports);
}

static void
test_open_disabled_reports(void) {
    scope_fixture();
    begin_face();
    ui_disabled_begin(true);
    (void)ui_end(UI_NO_BACKGROUND);
    TEST_ASSERT_EQUAL_INT(1, reports);
}

static void
test_open_color_reports(void) {
    scope_fixture();
    begin_face();
    ui_color_begin(UI_COLOR_TEXT, INK_RGB);
    (void)ui_end(UI_NO_BACKGROUND);
    TEST_ASSERT_EQUAL_INT(1, reports);
}

static void
test_open_scale_reports(void) {
    scope_fixture();
    begin_face();
    ui_scale_begin(SCOPE_SCALE);
    (void)ui_end(UI_NO_BACKGROUND);
    TEST_ASSERT_EQUAL_INT(1, reports);
}

static void
test_balanced_nested_scopes(void) {
    scope_fixture();
    begin_face();
    ui_disabled_begin(false);
    ui_disabled_begin(false);
    ui_color_begin(UI_COLOR_TEXT, INK_RGB);
    ui_color_begin(UI_COLOR_FACE, FACE_RGB);
    ui_scale_begin(SCOPE_SCALE);
    ui_scale_begin(THEME.text_scale);
    ui_text(BOX, "Balanced");
    ui_scale_end();
    ui_scale_end();
    ui_color_end();
    ui_color_end();
    ui_disabled_end();
    ui_disabled_end();
    (void)ui_end(UI_NO_BACKGROUND);
    TEST_ASSERT_EQUAL_INT(0, reports);
    TEST_ASSERT_TRUE(ui_probe_text("Balanced", &(recti_t){0}));
}

static void
test_unmatched_disabled_end_reports(void) {
    scope_fixture();
    begin_face();
    ui_disabled_end();
    (void)ui_end(UI_NO_BACKGROUND);
    TEST_ASSERT_EQUAL_INT(1, reports);
}

static void
test_unmatched_color_end_reports(void) {
    scope_fixture();
    begin_face();
    ui_color_end();
    (void)ui_end(UI_NO_BACKGROUND);
    TEST_ASSERT_EQUAL_INT(1, reports);
}

static void
test_open_alpha_reports(void) {
    scope_fixture();
    begin_face();
    ui_alpha_begin(FADE_ALPHA);
    (void)ui_end(UI_NO_BACKGROUND);
    TEST_ASSERT_EQUAL_INT(1, reports);
}

static void
test_unmatched_alpha_end_reports(void) {
    scope_fixture();
    begin_face();
    ui_alpha_end();
    (void)ui_end(UI_NO_BACKGROUND);
    TEST_ASSERT_EQUAL_INT(1, reports);
}

static void
test_unmatched_scale_end_reports(void) {
    scope_fixture();
    begin_face();
    ui_scale_end();
    (void)ui_end(UI_NO_BACKGROUND);
    TEST_ASSERT_EQUAL_INT(1, reports);
}
#endif
static void
test_toggle_theme_scopes(void) {
    fixture();
    begin_face();
    ui_theme(&THEME);
    const mu_Style before = *ui_context()->style;
    bool on = true;
    (void)ui_toggle(BOX, "Custom", &on);
    TEST_ASSERT_EQUAL_MEMORY(&before, ui_context()->style, sizeof before);
    ui_color_begin(UI_COLOR_TEXT, FACE_RGB);
    (void)ui_toggle(BOX, "Ink", &on);
    ui_scale_begin(SCOPE_SCALE);
    ui_disabled_begin(true);
    (void)ui_toggle(SECOND, "Scoped##key", &on);
    ui_disabled_end();
    ui_scale_end();
    ui_color_end();
    end_face();
    assert_ink("Custom", THEME.text);
    assert_ink("Ink", FACE_RGB);
    assert_ink("Scoped", THEME.muted);
    TEST_ASSERT_EQUAL_INT(draw_font_height(draw_font_ui(), SCOPE_SCALE), text_height_of("Scoped"));
    TEST_ASSERT_TRUE(has_face(THEME.button_face));
}

static void
test_ui_theme_custom_and_reset(void) {
    fixture();
    begin_legacy();
    const ui_mu_theme_t shell = ui_theme_from_style(ui_context()->style);
    const ui_mu_theme_t custom = legacy_theme(THEME);
    const ui_widget_button_t b = {.label = "Custom", .enabled = true};
    const ui_widget_button_t rest = {.label = "Shell", .enabled = true};
    (void)ui_icon_button(ui_context(), "Custom", ui_mu_from_recti(BOX), &b, &custom);
    (void)ui_icon_button(ui_context(), "Shell", ui_mu_from_recti(SECOND), &rest, &shell);
    end_legacy();
    save_commands();
    ui_init();
    begin_face();
    ui_theme(&THEME);
    (void)ui_button(BOX, "Custom");
    ui_theme(NULL);
    (void)ui_button(SECOND, "Shell");
    end_face();
    compare_commands();
}

/* With no ui_theme() call, captions, disabled labels and a header title all
 * draw in ink a reader can see. */
static void
test_the_shell_look_draws_every_text_visibly(void) {
    fixture();
    begin_face();
    ui_caption(BOX, "Caption");
    ui_disabled_begin(true);
    (void)ui_button(SECOND, "Muted");
    ui_disabled_end();
    ui_header((recti_t){0, 0, ui_width(), UI_TITLE_HEIGHT}, "Title");
    end_face();
    TEST_ASSERT_EQUAL_INT(UINT8_MAX, ui_probe_text_alpha("Caption"));
    TEST_ASSERT_EQUAL_INT(UINT8_MAX, ui_probe_text_alpha("Muted"));
    TEST_ASSERT_EQUAL_INT(UINT8_MAX, ui_probe_text_alpha("Title"));
}

/* With no ui_theme() call, an icon button's icon is the shell's icon side,
 * inset from the button's edge by the same padding its label keeps. */
static void
test_the_shell_look_insets_a_button_icon(void) {
    fixture();
    begin_face();
    (void)ui_button_icon(BOX, "Go", &ICON);
    end_face();
    const ui_theme_t any = {0};
    const int pad = (BOX.w - ui_probe_button_label_width(BOX.w, false, &any)) / 2;
    const mu_Color ink = ui_rgb(UI_SHELL_TEXT);
    recti_t icon = {0, 0, 0, 0};
    mu_Command* cmd = NULL;
    while (mu_next_command(ui_context(), &cmd)) {
        if (cmd->type == UI_COMMAND_IMAGE) {
            const ui_image_command_t* image = (const ui_image_command_t*)cmd;
            TEST_ASSERT_EQUAL_MEMORY(&ink, &image->color, sizeof ink);
            icon = recti_union(icon, ui_recti_from_mu(image->rect));
        }
    }
    const recti_t square = {BOX.x + pad, BOX.y + (BOX.h - UI_SHELL_ICON_SIDE) / 2, UI_SHELL_ICON_SIDE,
                            UI_SHELL_ICON_SIDE};
    TEST_ASSERT_FALSE(recti_is_empty(icon));
    TEST_ASSERT_TRUE(recti_contains(square, icon.x, icon.y));
    TEST_ASSERT_TRUE(recti_contains(square, recti_right(icon) - 1, recti_bottom(icon) - 1));
}

/* A dropdown without icons keeps no icon's room: its label is centred in the
 * slot between the padding and the chevron, at any theme's icon side. */
static void
test_a_dropdown_without_icons_centres_its_label_past_no_icon(void) {
    static const char* const NAMES[] = {"One"};
    int index = 0;
    fixture();
    begin_face();
    (void)ui_dropdown(BOX, "pick", &index, NAMES, 1);
    end_face();
    const ui_theme_t shell = {.icon_side = UI_SHELL_ICON_SIDE};
    const int pad = (BOX.w - ui_probe_button_label_width(BOX.w, false, &shell)) / 2;
    const int slot = ui_probe_dropdown_label_width(BOX.w, false, &shell);
    recti_t text;
    TEST_ASSERT_TRUE(ui_probe_text("One", &text));
    TEST_ASSERT_INT_WITHIN(1, BOX.x + pad + slot / 2, text.x + text.w / 2);
}

static void
test_ui_screen_twice_closes_first(void) {
    fixture();
    ui_begin(NULL);
    ui_screen();
    ui_text(BOX, "First");
    ui_screen();
    ui_text(SECOND, "Second");
    end_face();
    TEST_ASSERT_EQUAL_INT(2, ui_context()->root_list.idx);
    TEST_ASSERT_EQUAL_INT(0, ui_context()->container_stack.idx);
    TEST_ASSERT_TRUE(ui_probe_text("First", &(recti_t){0}));
    TEST_ASSERT_TRUE(ui_probe_text("Second", &(recti_t){0}));
}

void
run_ui_face_suite(void) {
    static const struct {
        verb_t verb;
        const char* name;
    } commands[] = {
        {VERB_BUTTON, "test_ui_button_commands"},
        {VERB_BUTTON_ICON, "test_ui_button_icon_commands"},
        {VERB_TILE, "test_ui_tile_commands"},
        {VERB_TILE_ICON, "test_ui_tile_icon_commands"},
        {VERB_TOGGLE, "test_ui_toggle_commands"},
        {VERB_SLIDER, "test_ui_slider_commands"},
        {VERB_DROPDOWN, "test_ui_dropdown_commands"},
        {VERB_DROPDOWN_ICONS, "test_ui_dropdown_icons_commands"},
        {VERB_TEXT, "test_ui_text_commands"},
        {VERB_TEXT_RIGHT, "test_ui_text_right_commands"},
        {VERB_TEXT_CENTRE, "test_ui_text_centre_commands"},
        {VERB_CAPTION, "test_ui_caption_commands"},
        {VERB_CAPTION_CENTRE, "test_ui_caption_centre_commands"},
        {VERB_HEADER, "test_ui_header_commands"},
        {VERB_PANEL, "test_ui_panel_commands"},
        {VERB_FILL, "test_ui_fill_commands"},
        {VERB_OUTLINE, "test_ui_outline_commands"},
        {VERB_ICON, "test_ui_icon_commands"},
        {VERB_BEVEL, "test_ui_bevel_commands"},
        {VERB_CHECK, "test_ui_check_commands"},
        {VERB_SWATCHES, "test_ui_swatches_commands"},
    };

    for (size_t i = 0; i < sizeof commands / sizeof commands[0]; i++) {
        command_verb = commands[i].verb;
        suite_run_test_timed(test_verb_commands, commands[i].name, __LINE__);
    }

    RUN_TEST(test_ui_screen_commands);
    RUN_TEST(test_ui_window_commands);
    RUN_TEST(test_ui_row_and_next);
    RUN_TEST(test_ui_row_centre_and_next);
    RUN_TEST(test_ui_row_centre_ends_with_window);
    RUN_TEST(test_ui_scroll_window_commands);
    RUN_TEST(test_ui_scroll_window_scrolls_vertically);
    RUN_TEST(test_identity_duplicate_label);
    RUN_TEST(test_identity_two_windows);
    RUN_TEST(test_identity_hidden_labels);
    RUN_TEST(test_identity_dynamic_label);
    RUN_TEST(test_identity_across_frames);
    RUN_TEST(test_ui_button_hit);
    RUN_TEST(test_ui_tile_hit);
    RUN_TEST(test_ui_icon_widgets_hit);
    RUN_TEST(test_ui_tile_icon_rect_is_the_tiles_own);
    RUN_TEST(test_overflowing_labels_clip_at_the_control_edges);
    RUN_TEST(test_ui_bevel_raised_matches_legacy_and_leaves_the_face);
    RUN_TEST(test_ui_toggle_hit);
    RUN_TEST(test_ui_slider_drag);
    RUN_TEST(test_ui_dropdown_pick);
    RUN_TEST(test_ui_dropdown_icons_pick);
    RUN_TEST(test_disabled_true_is_muted);
    RUN_TEST(test_disabled_false_is_unchanged);
    RUN_TEST(test_disabled_nested_false_stays_disabled);
    RUN_TEST(test_color_face_scope);
    RUN_TEST(test_color_text_scope);
    RUN_TEST(test_scale_scope);
    RUN_TEST(test_text_height_follows_scale);
    RUN_TEST(test_alpha_scope);
    RUN_TEST(test_alpha_scales_ink_coverage);
#ifndef ESP_PLATFORM
    RUN_TEST(test_scope_depth_boundary);
    RUN_TEST(test_nested_scope_values);
    RUN_TEST(test_disabled_every_widget);
    RUN_TEST(test_tap_release_contract);
    RUN_TEST(test_a_tap_just_outside_fires_the_nearest_control);
    RUN_TEST(test_a_press_that_slides_off_cancels_only_beyond_the_reach);
    RUN_TEST(test_unchanged_picks_return_false);
    RUN_TEST(test_duplicate_toggle_identity);
    RUN_TEST(test_duplicate_dropdown_identity);
    RUN_TEST(test_empty_visible_labels);
    RUN_TEST(test_frame_state_reset);
    RUN_TEST(test_all_end_paths);
    RUN_TEST(test_window_order_contract);
    RUN_TEST(test_additional_command_states);
    RUN_TEST(test_identity_capacity_reports);
    RUN_TEST(test_open_disabled_reports);
    RUN_TEST(test_open_color_reports);
    RUN_TEST(test_open_scale_reports);
    RUN_TEST(test_balanced_nested_scopes);
    RUN_TEST(test_unmatched_disabled_end_reports);
    RUN_TEST(test_unmatched_color_end_reports);
    RUN_TEST(test_unmatched_scale_end_reports);
    RUN_TEST(test_open_alpha_reports);
    RUN_TEST(test_unmatched_alpha_end_reports);
#endif
    RUN_TEST(test_toggle_theme_scopes);
    RUN_TEST(test_ui_theme_custom_and_reset);
    RUN_TEST(test_the_shell_look_draws_every_text_visibly);
    RUN_TEST(test_the_shell_look_insets_a_button_icon);
    RUN_TEST(test_a_dropdown_without_icons_centres_its_label_past_no_icon);
    RUN_TEST(test_ui_screen_twice_closes_first);
}

SUITE_REGISTER(run_ui_face_suite);
