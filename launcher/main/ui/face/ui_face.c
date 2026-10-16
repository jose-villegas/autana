/* Plain-value UI verbs share the microui control and command builders. */
#include "ui/ui.h"
#include "ui/ui_internal.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define UI_FACE_IDS_MAX      32
#define UI_FACE_SCOPE_DEPTH  4
#define UI_FACE_WINDOW_FLAGS (MU_OPT_NOTITLE | MU_OPT_NORESIZE | MU_OPT_NOCLOSE | MU_OPT_NOFRAME)
#define UI_FACE_NAME_MAX     32
#define UI_FACE_SLIDER_STEP  1

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
    ui_color_slot_t slot;
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
    if (!t) {
        theme = ui_theme_from_style(ui_context()->style);
        return;
    }
    theme = (ui_mu_theme_t){.panel_face = ui_rgb(t->panel_face),
                            .panel_edge = ui_rgb(t->panel_edge),
                            .button_face = ui_rgb(t->button_face),
                            .accent = ui_rgb(t->accent),
                            .accent_face = ui_rgb(t->accent_face),
                            .on_accent = ui_rgb(t->on_accent),
                            .text = ui_rgb(t->text),
                            .caption = ui_rgb(t->caption),
                            .muted = ui_rgb(t->muted),
                            .text_scale = t->text_scale,
                            .icon_side = t->icon_side};
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
    const char* key = strstr(label, "###");
    key = key ? key + 3 : label;
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
    const char* end = strstr(label, "##");
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
    mu_Context* ctx = ui_context();
    if (window_open) {
        mu_end_window(ctx);
    }
    char name[UI_FACE_NAME_MAX];
    snprintf(name, sizeof name, "ui.face.%u", window_ordinal++);
    mu_Container* cnt = mu_get_container(ctx, name);
    cnt->rect = ui_mu_from_recti(r);
    window_open = mu_begin_window_ex(ctx, name, cnt->rect, UI_FACE_WINDOW_FLAGS) != 0;
    id_count = 0;
}

void
ui_screen(void) {
    open_window((recti_t){0, 0, ui_width(), ui_height()});
}

void
ui_window(recti_t r) {
    open_window(r);
}

void
ui_row(int h) {
    const int widths[] = {-1};
    mu_layout_row(ui_context(), 1, widths, h);
}

recti_t
ui_next(void) {
    return ui_recti_from_mu(mu_layout_next(ui_context()));
}

static bool
button(recti_t r, const char* label, bool selected, bool tile) {
    const mu_Id id = widget_id(label);
    char* allocated;
    const ui_widget_button_t b = {
        .label = visible_label(label, &allocated), .enabled = !is_disabled(), .selected = selected};
    const bool hit = tile ? ui_tile_button_id(ui_context(), id, ui_mu_from_recti(r), &b, &theme)
                          : ui_icon_button_id(ui_context(), id, ui_mu_from_recti(r), &b, &theme);
    free(allocated);
    return hit;
}

bool
ui_button(recti_t r, const char* label) {
    return button(r, label, false, false);
}

bool
ui_tile(recti_t r, const char* label, bool selected) {
    return button(r, label, selected, true);
}

bool
ui_toggle(recti_t r, const char* label, bool* on) {
    const mu_Id id = widget_id(label);
    char* allocated;
    const char* text = visible_label(label, &allocated);
    int state = *on;
    mu_layout_set_next(ui_context(), ui_mu_from_recti(r), 0);
    const bool hit = mu_checkbox_id(ui_context(), text, &state, id, is_disabled() ? MU_OPT_NOINTERACT : 0) != 0;
    *on = state != 0;
    free(allocated);
    return hit;
}

bool
ui_slider(recti_t r, const char* label, int* value, int lo, int hi) {
    return ui_theme_slider_int_id(ui_context(), widget_id(label), ui_mu_from_recti(r), value, lo, hi,
                                  UI_FACE_SLIDER_STEP, !is_disabled(), &theme);
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
    snprintf(name, sizeof name, "ui.dropdown.%u", id);
    ui_dropdown_item_t* items = malloc((size_t)count * sizeof *items);
    assert(items);
    if (!items) {
        return false;
    }
    for (int i = 0; i < count; i++) {
        items[i] = (ui_dropdown_item_t){icons ? icons[i] : NULL, names[i]};
    }
    const int picked =
        ui_dropdown_id(ui_context(), id, name, ui_mu_from_recti(r), items, count, *index, !is_disabled(), &theme);
    free(items);
    if (picked < 0) {
        return false;
    }
    *index = picked;
    return true;
}

bool
ui_dropdown(recti_t r, const char* label, int* index, const char* const* names, int count) {
    return dropdown(r, label, index, names, NULL, count);
}

bool
ui_dropdown_icons(recti_t r, const char* label, int* index, const char* const* names, const draw_image_t* const* icons,
                  int count) {
    return dropdown(r, label, index, names, icons, count);
}

void
ui_text(recti_t r, const char* s) {
    ui_text_in(ui_context(), ui_mu_from_recti(r), s, theme.text, theme.text_scale, UI_ALIGN_LEFT);
}

void
ui_text_right(recti_t r, const char* s) {
    ui_text_in(ui_context(), ui_mu_from_recti(r), s, theme.text, theme.text_scale, UI_ALIGN_RIGHT);
}

void
ui_text_centre(recti_t r, const char* s) {
    ui_text_in(ui_context(), ui_mu_from_recti(r), s, theme.text, theme.text_scale, UI_ALIGN_CENTRE);
}

void
ui_caption(recti_t r, const char* s) {
    ui_text_in(ui_context(), ui_mu_from_recti(r), s, theme.caption, theme.text_scale, UI_ALIGN_LEFT);
}

void
ui_header(recti_t r, const char* title) {
    ui_header_bar(ui_context(), ui_mu_from_recti(r), title, theme.text_scale, &theme);
}

void
ui_panel(recti_t r) {
    ui_mu_panel(ui_context(), ui_mu_from_recti(r), &theme);
}

void
ui_fill(recti_t r, uint32_t rgb) {
    mu_draw_rect(ui_context(), ui_mu_from_recti(r), ui_rgb(rgb));
}

void
ui_outline(recti_t r, uint32_t rgb) {
    mu_draw_box(ui_context(), ui_mu_from_recti(r), ui_rgb(rgb));
}

void
ui_icon(recti_t r, const draw_image_t* icon) {
    ui_draw_icon(ui_context(), ui_mu_from_recti(r), icon, NULL, theme.text);
}

void
ui_swatches(recti_t r, const uint32_t* rgb, int cols, int rows) {
    if (cols <= 0 || rows <= 0) {
        return;
    }
    const size_t count = (size_t)cols * (size_t)rows;
    mu_Color* swatches = malloc(count * sizeof *swatches);
    assert(swatches);
    if (!swatches) {
        return;
    }
    for (size_t i = 0; i < count; i++) {
        swatches[i] = ui_rgb(rgb[i]);
    }
    ui_swatch_grid(ui_context(), ui_mu_from_recti(r), swatches, cols, rows);
    free(swatches);
}

void
ui_disabled_begin(bool value) {
    if (disabled_depth < UI_FACE_SCOPE_DEPTH) {
        disabled[disabled_depth] = value;
    } else {
        imbalance = true;
    }
    disabled_depth++;
}

void
ui_disabled_end(void) {
    if (disabled_depth > 0) {
        disabled_depth--;
    } else {
        imbalance = true;
    }
}

void
ui_color_begin(ui_color_slot_t slot, uint32_t rgb) {
    if (color_depth < UI_FACE_SCOPE_DEPTH) {
        colors[color_depth].slot = slot;
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
}

void
ui_color_end(void) {
    if (color_depth <= 0) {
        imbalance = true;
        return;
    }
    if (--color_depth < UI_FACE_SCOPE_DEPTH) {
        theme.text = colors[color_depth].text;
        theme.caption = colors[color_depth].caption;
        theme.button_face = colors[color_depth].face;
        theme.on_accent = colors[color_depth].selected;
    }
}

void
ui_scale_begin(int scale) {
    if (scale_depth < UI_FACE_SCOPE_DEPTH) {
        scales[scale_depth] = theme.text_scale;
        theme.text_scale = scale;
    } else {
        imbalance = true;
    }
    scale_depth++;
}

void
ui_scale_end(void) {
    if (scale_depth <= 0) {
        imbalance = true;
        return;
    }
    if (--scale_depth < UI_FACE_SCOPE_DEPTH) {
        theme.text_scale = scales[scale_depth];
    }
}
