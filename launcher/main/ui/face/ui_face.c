/* Plain-value UI verbs share the microui control and command builders. */
#include "profile/frame_cost.h"
#include "ui/ui.h"
#include "ui/ui_internal.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ui/ui_mu.h"

#define UI_FACE_IDS_MAX              32
#define UI_FACE_SCOPE_DEPTH          4
#define UI_FACE_WINDOW_FLAGS         (MU_OPT_NOTITLE | MU_OPT_NORESIZE | MU_OPT_NOCLOSE | MU_OPT_NOFRAME)
#define UI_FACE_NAME_MAX             32
#define UI_FACE_SLIDER_STEP          1
#define UI_FACE_LABEL_ID_MARKER      "##"
#define UI_FACE_LABEL_KEY_MARKER     "###"
#define UI_FACE_WINDOW_NAME_FORMAT   "ui.face.%u"
#define UI_FACE_DROPDOWN_NAME_FORMAT "ui.dropdown.%u"

static ui_mu_theme_t theme;
static unsigned window_ordinal;
static bool window_open;

static struct {
    mu_Id hash;
    unsigned count;
} ids[UI_FACE_IDS_MAX];

static int id_count;
static bool disabled[UI_FACE_SCOPE_DEPTH];

static struct {
    mu_Color text, caption, face, selected;
} colors[UI_FACE_SCOPE_DEPTH];

static int scales[UI_FACE_SCOPE_DEPTH];
static int disabled_depth, color_depth, scale_depth;
static bool imbalance;

#ifndef ESP_PLATFORM
void (*ui_scope_imbalance_for_test)(void);
#endif

void
ui_face_begin(void) {
    theme = ui_theme_from_style(ui_context()->style);
    window_ordinal = 0;
    window_open = false;
    id_count = 0;
    disabled_depth = color_depth = scale_depth = 0;
    imbalance = false;
}

void
ui_face_end(void) {
    if (window_open) {
        mu_end_window(ui_context());
        window_open = false;
    }
#if !defined(ESP_PLATFORM) || CONFIG_LAUNCHER_DEVELOPMENT
    const bool balanced = !imbalance && disabled_depth == 0 && color_depth == 0 && scale_depth == 0;
#ifndef ESP_PLATFORM
    if (!balanced && ui_scope_imbalance_for_test) {
        ui_scope_imbalance_for_test();
    }
#else
    assert(balanced);
#endif
#endif
    disabled_depth = color_depth = scale_depth = 0;
    imbalance = false;
}

void
ui_theme(const ui_theme_t* t) {
    FRAME_COST_BEGIN(face_from);
    if (!t) {
        theme = ui_theme_from_style(ui_context()->style);
        FRAME_COST_END(face_from, "ui.face");
        return;
    }
#define UI_FACE_THEME_COLOR(member) .member = ui_rgb(t->member)
    theme = (ui_mu_theme_t){
        UI_FACE_THEME_COLOR(panel_face), UI_FACE_THEME_COLOR(panel_edge),  UI_FACE_THEME_COLOR(button_face),
        UI_FACE_THEME_COLOR(accent),     UI_FACE_THEME_COLOR(accent_face), UI_FACE_THEME_COLOR(on_accent),
        UI_FACE_THEME_COLOR(text),       UI_FACE_THEME_COLOR(caption),     UI_FACE_THEME_COLOR(muted),
        .text_scale = t->text_scale,     .icon_side = t->icon_side};
    FRAME_COST_END(face_from, "ui.face");
}

static bool
is_disabled(void) {
    for (int i = 0; i < disabled_depth && i < UI_FACE_SCOPE_DEPTH; i++) {
        if (disabled[i]) {
            return true;
        }
    }
    return false;
}

static mu_Id
widget_id(const char* label) {
    const char* key = strstr(label, UI_FACE_LABEL_KEY_MARKER);
    key = key ? key + sizeof(UI_FACE_LABEL_KEY_MARKER) - 1 : label;
    const mu_Id hash = mu_get_id(ui_context(), key, (int)strlen(key));
    unsigned occurrence = 0;
    int i;
    for (i = 0; i < id_count; i++) {
        if (ids[i].hash == hash) {
            occurrence = ids[i].count++;
            break;
        }
    }
    if (i == id_count) {
        if (id_count < UI_FACE_IDS_MAX) {
            ids[id_count].hash = hash;
            ids[id_count++].count = 1;
        } else {
#if CONFIG_LAUNCHER_DEVELOPMENT
            assert(id_count < UI_FACE_IDS_MAX);
#endif
        }
    }
    const unsigned identity[] = {window_ordinal, hash, occurrence};
    return mu_get_id(ui_context(), identity, sizeof identity);
}

static const char*
visible_label(const char* label, char** allocated) {
    const char* end = strstr(label, UI_FACE_LABEL_ID_MARKER);
    *allocated = NULL;
    if (!end) {
        return label;
    }
    const size_t n = (size_t)(end - label);
    *allocated = malloc(n + 1);
    assert(*allocated);
    if (!*allocated) {
        return "";
    }
    memcpy(*allocated, label, n);
    (*allocated)[n] = '\0';
    return *allocated;
}

static void
open_window(recti_t r) {
    char name[UI_FACE_NAME_MAX];
    snprintf(name, sizeof name, UI_FACE_WINDOW_NAME_FORMAT, window_ordinal++);
    id_count = 0;
    mu_Context* ctx = ui_context();
    FRAME_COST_BEGIN(backend_from);
    if (window_open) {
        mu_end_window(ctx);
    }
    mu_Container* cnt = mu_get_container(ctx, name);
    cnt->rect = ui_mu_from_recti(r);
    window_open = mu_begin_window_ex(ctx, name, cnt->rect, UI_FACE_WINDOW_FLAGS) != 0;
    FRAME_COST_END(backend_from, "ui.backend");
}

void
ui_screen(void) {
    FRAME_COST_BEGIN(face_from);
    open_window((recti_t){0, 0, ui_width(), ui_height()});
    FRAME_COST_END(face_from, "ui.face");
}

void
ui_window(recti_t r) {
    FRAME_COST_BEGIN(face_from);
    open_window(r);
    FRAME_COST_END(face_from, "ui.face");
}

void
ui_row(int h) {
    FRAME_COST_BEGIN(face_from);
    FRAME_COST_BEGIN(backend_from);
    const int widths[] = {-1};
    mu_layout_row(ui_context(), 1, widths, h);
    FRAME_COST_END(backend_from, "ui.backend");
    FRAME_COST_END(face_from, "ui.face");
}

recti_t
ui_next(void) {
    FRAME_COST_BEGIN(face_from);
    FRAME_COST_BEGIN(backend_from);
    const recti_t result = ui_recti_from_mu(mu_layout_next(ui_context()));
    FRAME_COST_END(backend_from, "ui.backend");
    const recti_t face_result = result;
    FRAME_COST_END(face_from, "ui.face");
    return face_result;
}

static bool
button(recti_t r, const char* label, bool selected, bool tile) {
    const mu_Id id = widget_id(label);
    char* allocated;
    const ui_widget_button_t b = {
        .label = visible_label(label, &allocated), .enabled = !is_disabled(), .selected = selected};
    FRAME_COST_BEGIN(backend_from);
    const bool hit = tile ? ui_tile_button_id(ui_context(), id, ui_mu_from_recti(r), &b, &theme)
                          : ui_icon_button_id(ui_context(), id, ui_mu_from_recti(r), &b, &theme);
    FRAME_COST_END(backend_from, "ui.backend");
    free(allocated);
    return hit;
}

bool
ui_button(recti_t r, const char* label) {
    FRAME_COST_BEGIN(face_from);
    const bool face_result = button(r, label, false, false);
    FRAME_COST_END(face_from, "ui.face");
    return face_result;
}

bool
ui_tile(recti_t r, const char* label, bool selected) {
    FRAME_COST_BEGIN(face_from);
    const bool face_result = button(r, label, selected, true);
    FRAME_COST_END(face_from, "ui.face");
    return face_result;
}

bool
ui_toggle(recti_t r, const char* label, bool* on) {
    FRAME_COST_BEGIN(face_from);
    const mu_Id id = widget_id(label);
    char* allocated;
    const char* text = visible_label(label, &allocated);
    int state = *on;
    const int opt = is_disabled() ? MU_OPT_NOINTERACT : 0;
    FRAME_COST_BEGIN(backend_from);
    mu_layout_set_next(ui_context(), ui_mu_from_recti(r), 0);
    const bool hit = mu_checkbox_id(ui_context(), text, &state, id, opt) != 0;
    FRAME_COST_END(backend_from, "ui.backend");
    *on = state != 0;
    free(allocated);
    const bool face_result = hit;
    FRAME_COST_END(face_from, "ui.face");
    return face_result;
}

bool
ui_slider(recti_t r, const char* label, int* value, int lo, int hi) {
    FRAME_COST_BEGIN(face_from);
    const mu_Id id = widget_id(label);
    const bool enabled = !is_disabled();
    FRAME_COST_BEGIN(backend_from);
    const bool result = ui_theme_slider_int_id(ui_context(), id, ui_mu_from_recti(r), value, lo, hi,
                                               UI_FACE_SLIDER_STEP, enabled, &theme);
    FRAME_COST_END(backend_from, "ui.backend");
    const bool face_result = result;
    FRAME_COST_END(face_from, "ui.face");
    return face_result;
}

static bool
dropdown(recti_t r, const char* label, int* index, const char* const* names, const draw_image_t* const* icons,
         int count) {
    assert(count > 0);
    if (count <= 0) {
        return false;
    }
    const mu_Id id = widget_id(label);
    char name[UI_FACE_NAME_MAX];
    snprintf(name, sizeof name, UI_FACE_DROPDOWN_NAME_FORMAT, id);
    ui_dropdown_item_t* items = malloc((size_t)count * sizeof *items);
    assert(items);
    if (!items) {
        return false;
    }
    for (int i = 0; i < count; i++) {
        items[i] = (ui_dropdown_item_t){icons ? icons[i] : NULL, names[i]};
    }
    const bool enabled = !is_disabled();
    FRAME_COST_BEGIN(backend_from);
    const int picked =
        ui_dropdown_id(ui_context(), id, name, ui_mu_from_recti(r), items, count, *index, enabled, &theme);
    FRAME_COST_END(backend_from, "ui.backend");
    free(items);
    if (picked < 0) {
        return false;
    }
    *index = picked;
    return true;
}

bool
ui_dropdown(recti_t r, const char* label, int* index, const char* const* names, int count) {
    FRAME_COST_BEGIN(face_from);
    const bool face_result = dropdown(r, label, index, names, NULL, count);
    FRAME_COST_END(face_from, "ui.face");
    return face_result;
}

bool
ui_dropdown_icons(recti_t r, const char* label, int* index, const char* const* names, const draw_image_t* const* icons,
                  int count) {
    FRAME_COST_BEGIN(face_from);
    const bool face_result = dropdown(r, label, index, names, icons, count);
    FRAME_COST_END(face_from, "ui.face");
    return face_result;
}

#define UI_FACE_TEXT(name, alignment, ink)                                                                             \
    void name(recti_t r, const char* s) {                                                                              \
        FRAME_COST_BEGIN(face_from);                                                                                   \
        FRAME_COST_BEGIN(backend_from);                                                                                \
        ui_text_in(ui_context(), ui_mu_from_recti(r), s, theme.ink, theme.text_scale, alignment);                      \
        FRAME_COST_END(backend_from, "ui.backend");                                                                    \
        FRAME_COST_END(face_from, "ui.face");                                                                          \
    }
UI_FACE_TEXT(ui_text, UI_ALIGN_LEFT, text)
UI_FACE_TEXT(ui_text_right, UI_ALIGN_RIGHT, text)
UI_FACE_TEXT(ui_text_centre, UI_ALIGN_CENTRE, text)
UI_FACE_TEXT(ui_caption, UI_ALIGN_LEFT, caption)

void
ui_header(recti_t r, const char* title) {
    FRAME_COST_BEGIN(face_from);
    FRAME_COST_BEGIN(backend_from);
    ui_header_bar(ui_context(), ui_mu_from_recti(r), title, theme.text_scale, &theme);
    FRAME_COST_END(backend_from, "ui.backend");
    FRAME_COST_END(face_from, "ui.face");
}

void
ui_panel(recti_t r) {
    FRAME_COST_BEGIN(face_from);
    FRAME_COST_BEGIN(backend_from);
    ui_mu_panel(ui_context(), ui_mu_from_recti(r), &theme);
    FRAME_COST_END(backend_from, "ui.backend");
    FRAME_COST_END(face_from, "ui.face");
}

void
ui_fill(recti_t r, uint32_t rgb) {
    FRAME_COST_BEGIN(face_from);
    FRAME_COST_BEGIN(backend_from);
    mu_draw_rect(ui_context(), ui_mu_from_recti(r), ui_rgb(rgb));
    FRAME_COST_END(backend_from, "ui.backend");
    FRAME_COST_END(face_from, "ui.face");
}

void
ui_outline(recti_t r, uint32_t rgb) {
    FRAME_COST_BEGIN(face_from);
    FRAME_COST_BEGIN(backend_from);
    mu_draw_box(ui_context(), ui_mu_from_recti(r), ui_rgb(rgb));
    FRAME_COST_END(backend_from, "ui.backend");
    FRAME_COST_END(face_from, "ui.face");
}

void
ui_icon(recti_t r, const draw_image_t* icon) {
    FRAME_COST_BEGIN(face_from);
    FRAME_COST_BEGIN(backend_from);
    ui_draw_icon(ui_context(), ui_mu_from_recti(r), icon, NULL, theme.text);
    FRAME_COST_END(backend_from, "ui.backend");
    FRAME_COST_END(face_from, "ui.face");
}

void
ui_swatches(recti_t r, const uint32_t* rgb, int cols, int rows) {
    FRAME_COST_BEGIN(face_from);
    if (cols <= 0 || rows <= 0) {
        FRAME_COST_END(face_from, "ui.face");
        return;
    }
    const size_t count = (size_t)cols * (size_t)rows;
    mu_Color* swatches = malloc(count * sizeof *swatches);
    assert(swatches);
    if (!swatches) {
        FRAME_COST_END(face_from, "ui.face");
        return;
    }
    for (size_t i = 0; i < count; i++) {
        swatches[i] = ui_rgb(rgb[i]);
    }
    FRAME_COST_BEGIN(backend_from);
    ui_swatch_grid(ui_context(), ui_mu_from_recti(r), swatches, cols, rows);
    FRAME_COST_END(backend_from, "ui.backend");
    free(swatches);
    FRAME_COST_END(face_from, "ui.face");
}

void
ui_disabled_begin(bool value) {
    FRAME_COST_BEGIN(face_from);
    if (disabled_depth < UI_FACE_SCOPE_DEPTH) {
        disabled[disabled_depth] = value;
    } else {
        imbalance = true;
    }
    disabled_depth++;
    FRAME_COST_END(face_from, "ui.face");
}

void
ui_disabled_end(void) {
    FRAME_COST_BEGIN(face_from);
    if (disabled_depth > 0) {
        disabled_depth--;
    } else {
        imbalance = true;
    }
    FRAME_COST_END(face_from, "ui.face");
}

void
ui_color_begin(ui_color_slot_t slot, uint32_t rgb) {
    FRAME_COST_BEGIN(face_from);
    if (color_depth < UI_FACE_SCOPE_DEPTH) {
        colors[color_depth].text = theme.text;
        colors[color_depth].caption = theme.caption;
        colors[color_depth].face = theme.button_face;
        colors[color_depth].selected = theme.on_accent;
        if (slot == UI_COLOR_FACE) {
            theme.button_face = ui_rgb(rgb);
        } else {
            theme.text = theme.caption = theme.on_accent = ui_rgb(rgb);
        }
    } else {
        imbalance = true;
    }
    color_depth++;
    FRAME_COST_END(face_from, "ui.face");
}

void
ui_color_end(void) {
    FRAME_COST_BEGIN(face_from);
    if (color_depth <= 0) {
        imbalance = true;
        FRAME_COST_END(face_from, "ui.face");
        return;
    }
    if (--color_depth < UI_FACE_SCOPE_DEPTH) {
        theme.text = colors[color_depth].text;
        theme.caption = colors[color_depth].caption;
        theme.button_face = colors[color_depth].face;
        theme.on_accent = colors[color_depth].selected;
    }
    FRAME_COST_END(face_from, "ui.face");
}

void
ui_scale_begin(int scale) {
    FRAME_COST_BEGIN(face_from);
    if (scale_depth < UI_FACE_SCOPE_DEPTH) {
        scales[scale_depth] = theme.text_scale;
        theme.text_scale = scale;
    } else {
        imbalance = true;
    }
    scale_depth++;
    FRAME_COST_END(face_from, "ui.face");
}

void
ui_scale_end(void) {
    FRAME_COST_BEGIN(face_from);
    if (scale_depth <= 0) {
        imbalance = true;
        FRAME_COST_END(face_from, "ui.face");
        return;
    }
    if (--scale_depth < UI_FACE_SCOPE_DEPTH) {
        theme.text_scale = scales[scale_depth];
    }
    FRAME_COST_END(face_from, "ui.face");
}
