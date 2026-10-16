#include <stdlib.h>
#include <string.h>

#include "draw/draw_font_roles.h"
#include "gfx/gfx.h"
#ifndef ESP_PLATFORM
#include "gfx/present/gfx_test.h"
#endif
#include "suites.h"
#include "test_cleanup.h"
#include "timing.h"
#include "ui/ui.h"
#include "ui/ui_internal.h"
#include "ui/ui_mu.h"
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

static void
cleanup(void) {
    free(snapshot);
    snapshot = NULL;
#ifndef ESP_PLATFORM
    ui_scope_imbalance_for_test = NULL;
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
    ui_set_transform(ui_transform_identity());
    snapshot_size = 0;
    reports = 0;
}

static mu_Rect
legacy_rect(recti_t r) {
    return mu_rect(r.x, r.y, r.w, r.h);
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
static const draw_image_t ICON = {
    .bits = ICON_BITS, .format = DRAW_IMAGE_MONO1, .width = 1, .height = 1, .stride = DRAW_IMAGE_MONO1_STRIDE_STEP};
static const draw_image_t* const ICONS[] = {&ICON, NULL, &ICON};

typedef enum {
    VERB_BUTTON,
    VERB_TILE,
    VERB_TOGGLE,
    VERB_SLIDER,
    VERB_DROPDOWN,
    VERB_DROPDOWN_ICONS,
    VERB_TEXT,
    VERB_TEXT_RIGHT,
    VERB_TEXT_CENTRE,
    VERB_CAPTION,
    VERB_HEADER,
    VERB_PANEL,
    VERB_FILL,
    VERB_OUTLINE,
    VERB_ICON,
    VERB_SWATCHES
} verb_t;

static void
emit_legacy(verb_t verb) {
    mu_Context* ctx = ui_context();
    const mu_Rect r = legacy_rect(BOX);
    const ui_mu_theme_t t = legacy_theme(THEME);
    const ui_widget_button_t b = {.label = "Go", .enabled = true};
    int value = INITIAL_VALUE;
    int on = 0;
    const ui_dropdown_item_t items[] = {{NULL, "One"}, {NULL, "Two"}, {NULL, "Three"}};
    const ui_dropdown_item_t icons[] = {{&ICON, "One"}, {NULL, "Two"}, {&ICON, "Three"}};
    const mu_Color colors[] = {UI_RGB(FACE_RGB), UI_RGB(INK_RGB)};
    switch (verb) {
        case VERB_BUTTON: (void)ui_icon_button(ctx, "Go", r, &b, &t); break;
        case VERB_TILE: (void)ui_tile_button(ctx, "Go", r, &b, &t); break;
        case VERB_TOGGLE:
            mu_layout_set_next(ctx, r, 0);
            (void)mu_checkbox(ctx, "Go", &on);
            break;
        case VERB_SLIDER: (void)ui_theme_slider_int(ctx, r, &value, RANGE_LO, RANGE_HI, 1, &t); break;
        case VERB_DROPDOWN: (void)ui_mu_dropdown(ctx, "Go", r, items, CHOICE_COUNT, 0, &t); break;
        case VERB_DROPDOWN_ICONS: (void)ui_mu_dropdown(ctx, "Go", r, icons, CHOICE_COUNT, 0, &t); break;
        case VERB_TEXT:
        case VERB_TEXT_RIGHT:
        case VERB_TEXT_CENTRE:
        case VERB_CAPTION: {
            const ui_align_t align = verb == VERB_TEXT_RIGHT    ? UI_ALIGN_RIGHT
                                     : verb == VERB_TEXT_CENTRE ? UI_ALIGN_CENTRE
                                                                : UI_ALIGN_LEFT;
            ui_text_in(ctx, r, "Go", verb == VERB_CAPTION ? t.caption : t.text, t.text_scale, align);
            break;
        }
        case VERB_HEADER: ui_header_bar(ctx, r, "Go", t.text_scale, &t); break;
        case VERB_PANEL: ui_mu_panel(ctx, r, &t); break;
        case VERB_FILL: mu_draw_rect(ctx, r, ui_rgb(FACE_RGB)); break;
        case VERB_OUTLINE: mu_draw_box(ctx, r, ui_rgb(FACE_RGB)); break;
        case VERB_ICON: ui_draw_icon(ctx, r, &ICON, NULL, t.text); break;
        case VERB_SWATCHES: ui_swatch_grid(ctx, r, colors, SWATCH_COLS, SWATCH_ROWS); break;
    }
}

static bool
emit_face(verb_t verb, int* value, bool* on) {
    switch (verb) {
        case VERB_BUTTON: return ui_button(BOX, "Go");
        case VERB_TILE: return ui_tile(BOX, "Go", false);
        case VERB_TOGGLE: return ui_toggle(BOX, "Go", on);
        case VERB_SLIDER: return ui_slider(BOX, "Go", value, RANGE_LO, RANGE_HI);
        case VERB_DROPDOWN: return ui_dropdown(BOX, "Go", value, NAMES, CHOICE_COUNT);
        case VERB_DROPDOWN_ICONS: return ui_dropdown_icons(BOX, "Go", value, NAMES, ICONS, CHOICE_COUNT);
        case VERB_TEXT: ui_text(BOX, "Go"); break;
        case VERB_TEXT_RIGHT: ui_text_right(BOX, "Go"); break;
        case VERB_TEXT_CENTRE: ui_text_centre(BOX, "Go"); break;
        case VERB_CAPTION: ui_caption(BOX, "Go"); break;
        case VERB_HEADER: ui_header(BOX, "Go"); break;
        case VERB_PANEL: ui_panel(BOX); break;
        case VERB_FILL: ui_fill(BOX, FACE_RGB); break;
        case VERB_OUTLINE: ui_outline(BOX, FACE_RGB); break;
        case VERB_ICON: ui_icon(BOX, &ICON); break;
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
        ui_theme(&THEME);
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

static const mu_TextCommand*
text_command(const char* label) {
    mu_Command* cmd = NULL;
    while (mu_next_command(ui_context(), &cmd)) {
        if (cmd->type == MU_COMMAND_TEXT && strcmp(cmd->text.str, label) == 0) {
            return &cmd->text;
        }
    }
    return NULL;
}

static void
assert_ink(const char* label, uint32_t rgb) {
    const mu_TextCommand* text = text_command(label);
    TEST_ASSERT_NOT_NULL(text);
    const mu_Color color = ui_rgb(rgb);
    TEST_ASSERT_EQUAL_MEMORY(&color, &text->color, sizeof color);
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
    mu_draw_rect(ui_context(), legacy_rect(BOX), ui_rgb(FACE_RGB));
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
    TEST_ASSERT_TRUE(mu_begin_window_ex(ui_context(), "Reference", legacy_rect(BOX), WINDOW_FLAGS));
    mu_draw_rect(ui_context(), legacy_rect(BOX), ui_rgb(FACE_RGB));
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

typedef enum { ID_DUPLICATE, ID_WINDOWS, ID_HIDDEN, ID_DYNAMIC, ID_STABLE } identity_t;

static unsigned
identity_frame(identity_t kind, const input_t* in, bool changed) {
    ui_begin(in);
    ui_theme(&THEME);
    if (kind == ID_WINDOWS) {
        ui_window((recti_t){0, 0, ui_width(), SECOND.y});
    } else {
        ui_screen();
    }
    const char* first = kind == ID_HIDDEN ? "Go##a" : kind == ID_DYNAMIC ? (changed ? "B###k" : "A###k") : "Go";
    unsigned hits = ui_button(BOX, first) ? 1U : 0U;
    if (kind == ID_WINDOWS) {
        ui_window((recti_t){0, SECOND.y, ui_width(), ui_height() - SECOND.y});
    }
    if (kind == ID_DUPLICATE || kind == ID_WINDOWS || kind == ID_HIDDEN) {
        if (ui_button(SECOND, kind == ID_HIDDEN ? "Go##b" : "Go")) {
            hits |= 2U;
        }
    }
    end_face();
    return hits;
}

static input_t
touch_sample(recti_t r, bool down, bool edge) {
    const int x = r.x + r.w / 2, y = r.y + r.h / 2;
    return (input_t){
        .pressed = down && edge, .released = !down && edge, .down = down, .x = x, .y = y, .press_x = x, .press_y = y};
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

static bool
tap_widget(verb_t verb, int* value, bool* on, bool disabled, bool nested) {
    const input_t idle = {0};
    const input_t press = touch_sample(BOX, true, true);
    const input_t hold = touch_sample(BOX, true, false);
    const input_t release = touch_sample(BOX, false, true);
    (void)widget_frame(verb, &idle, value, on, disabled, nested);
    (void)widget_frame(verb, &idle, value, on, disabled, nested);
    bool hit = widget_frame(verb, &press, value, on, disabled, nested);
    for (int i = 0; i < TOUCH_HOLD_FRAMES; i++) {
        hit |= widget_frame(verb, &hold, value, on, disabled, nested);
    }
    hit |= widget_frame(verb, &release, value, on, disabled, nested);
    return hit;
}

static void
widget_hit(verb_t verb) {
    fixture();
    int value = INITIAL_VALUE;
    bool on = false;
    TEST_ASSERT_TRUE(tap_widget(verb, &value, &on, false, false));
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
test_ui_toggle_hit(void) {
    fixture();
    int value = INITIAL_VALUE;
    bool on = false;
    TEST_ASSERT_TRUE(tap_widget(VERB_TOGGLE, &value, &on, false, false));
    TEST_ASSERT_TRUE(on);
}

static void
test_ui_slider_drag(void) {
    fixture();
    int value = INITIAL_VALUE;
    bool on = false;
    const input_t idle = {0};
    const int y = BOX.y + BOX.h / 2;
    const input_t press = {.down = true, .pressed = true, .x = BOX.x + BOX.w / 4, .y = y};
    const input_t drag = {.down = true, .x = BOX.x + BOX.w * 3 / 4, .y = y};
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
    (void)tap_widget(verb, &value, &on, false, false);
    const int row_h = BOX.h;
    const mu_Rect list = ui_dropdown_list_rect(legacy_rect(BOX), CHOICE_COUNT, row_h, ui_height(), UI_MARGIN);
    const int x = list.x + list.w / 2, y = list.y + row_h + row_h / 2;
    const input_t idle = {0}, press = {.down = true, .pressed = true, .x = x, .y = y},
                  release = {.released = true, .x = x, .y = y};
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
    TEST_ASSERT_FALSE(tap_widget(VERB_BUTTON, &value, &on, true, false));
    assert_ink("Go", THEME.muted);
}

static void
test_disabled_false_is_unchanged(void) {
    fixture();
    int value = 0;
    bool on = false;
    TEST_ASSERT_TRUE(tap_widget(VERB_BUTTON, &value, &on, false, false));
    assert_ink("Go", THEME.text);
}

static void
test_disabled_nested_false_stays_disabled(void) {
    fixture();
    int value = 0;
    bool on = false;
    TEST_ASSERT_FALSE(tap_widget(VERB_BUTTON, &value, &on, true, true));
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
    const mu_TextCommand* text = text_command("Text");
    TEST_ASSERT_NOT_NULL(text);
    const int height = ui_context()->text_height(text->font);
    text = text_command("Label");
    TEST_ASSERT_NOT_NULL(text);
    TEST_ASSERT_EQUAL_INT(height, ui_context()->text_height(text->font));
    TEST_ASSERT_EQUAL_INT(draw_font_height(draw_font_ui(), SCOPE_SCALE), height);
    text = text_command("Rest");
    TEST_ASSERT_NOT_NULL(text);
    TEST_ASSERT_EQUAL_INT(draw_font_height(draw_font_ui(), THEME.text_scale), ui_context()->text_height(text->font));
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
    ui_scope_imbalance_for_test = report_imbalance;
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
    TEST_ASSERT_NOT_NULL(text_command("Balanced"));
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
test_unmatched_scale_end_reports(void) {
    scope_fixture();
    begin_face();
    ui_scale_end();
    (void)ui_end(UI_NO_BACKGROUND);
    TEST_ASSERT_EQUAL_INT(1, reports);
}
#endif
static void
test_ui_theme_custom_and_reset(void) {
    fixture();
    begin_legacy();
    const ui_mu_theme_t shell = ui_theme_from_style(ui_context()->style);
    const ui_mu_theme_t custom = legacy_theme(THEME);
    const ui_widget_button_t b = {.label = "Custom", .enabled = true};
    const ui_widget_button_t rest = {.label = "Shell", .enabled = true};
    (void)ui_icon_button(ui_context(), "Custom", legacy_rect(BOX), &b, &custom);
    (void)ui_icon_button(ui_context(), "Shell", legacy_rect(SECOND), &rest, &shell);
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
    TEST_ASSERT_NOT_NULL(text_command("First"));
    TEST_ASSERT_NOT_NULL(text_command("Second"));
}

void
run_ui_face_suite(void) {
    static const struct {
        verb_t verb;
        const char* name;
    } commands[] = {
        {VERB_BUTTON, "test_ui_button_commands"},
        {VERB_TILE, "test_ui_tile_commands"},
        {VERB_TOGGLE, "test_ui_toggle_commands"},
        {VERB_SLIDER, "test_ui_slider_commands"},
        {VERB_DROPDOWN, "test_ui_dropdown_commands"},
        {VERB_DROPDOWN_ICONS, "test_ui_dropdown_icons_commands"},
        {VERB_TEXT, "test_ui_text_commands"},
        {VERB_TEXT_RIGHT, "test_ui_text_right_commands"},
        {VERB_TEXT_CENTRE, "test_ui_text_centre_commands"},
        {VERB_CAPTION, "test_ui_caption_commands"},
        {VERB_HEADER, "test_ui_header_commands"},
        {VERB_PANEL, "test_ui_panel_commands"},
        {VERB_FILL, "test_ui_fill_commands"},
        {VERB_OUTLINE, "test_ui_outline_commands"},
        {VERB_ICON, "test_ui_icon_commands"},
        {VERB_SWATCHES, "test_ui_swatches_commands"},
    };

    for (size_t i = 0; i < sizeof commands / sizeof commands[0]; i++) {
        command_verb = commands[i].verb;
        suite_run_test_timed(test_verb_commands, commands[i].name, __LINE__);
    }

    RUN_TEST(test_ui_screen_commands);
    RUN_TEST(test_ui_window_commands);
    RUN_TEST(test_ui_row_and_next);
    RUN_TEST(test_identity_duplicate_label);
    RUN_TEST(test_identity_two_windows);
    RUN_TEST(test_identity_hidden_labels);
    RUN_TEST(test_identity_dynamic_label);
    RUN_TEST(test_identity_across_frames);
    RUN_TEST(test_ui_button_hit);
    RUN_TEST(test_ui_tile_hit);
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
#ifndef ESP_PLATFORM
    RUN_TEST(test_open_disabled_reports);
    RUN_TEST(test_open_color_reports);
    RUN_TEST(test_open_scale_reports);
    RUN_TEST(test_balanced_nested_scopes);
    RUN_TEST(test_unmatched_disabled_end_reports);
    RUN_TEST(test_unmatched_color_end_reports);
    RUN_TEST(test_unmatched_scale_end_reports);
#endif
    RUN_TEST(test_ui_theme_custom_and_reset);
    RUN_TEST(test_ui_screen_twice_closes_first);
}

SUITE_REGISTER(run_ui_face_suite);
