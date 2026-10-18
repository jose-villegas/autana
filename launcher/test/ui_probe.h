/* What a face frame built, for an app's suite that drives a screen through
 * the face: where a widget landed, where a line of text was drawn, and
 * whether a dropdown's list is open. Read from ui/'s private state, so the
 * suite itself includes only ui.h. */
#pragma once

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "core/memory.h"
#include "ui/ui.h"
#include "ui/ui_internal.h"
#include "ui/ui_mu.h"
#include "unity.h"

static inline const mu_TextCommand*
ui_probe_text_command(const char* s) {
    mu_Command* cmd = NULL;
    while (mu_next_command(ui_context(), &cmd)) {
        if (cmd->type == MU_COMMAND_TEXT && strcmp(cmd->text.str, s) == 0) {
            return &cmd->text;
        }
    }
    return NULL;
}

/* Where a line of text `s` was drawn in the last built frame, in canvas
 * coordinates: its first glyph's corner and its measured size. */
static inline bool
ui_probe_text(const char* s, recti_t* out) {
    const mu_TextCommand* text = ui_probe_text_command(s);
    if (text == NULL) {
        return false;
    }
    /* Measured from the font directly: the context's measure hooks are calls
     * through pointers the device's stack-chain gate cannot follow. */
    const ui_font_scaled_t fs = ui_resolve_font_scaled(text->font);
    *out = (recti_t){text->pos.x, text->pos.y, draw_font_text_width(fs.font, s, -1, fs.scale),
                     draw_font_height(fs.font, fs.scale)};
    return true;
}

/* The clip rectangle in force when text `s` was drawn. */
static inline bool
ui_probe_text_clip(const char* s, recti_t* out) {
    mu_Rect clip = {0};
    mu_Command* cmd = NULL;
    while (mu_next_command(ui_context(), &cmd)) {
        if (cmd->type == MU_COMMAND_CLIP) {
            clip = cmd->clip.rect;
        } else if (cmd->type == MU_COMMAND_TEXT && strcmp(cmd->text.str, s) == 0) {
            *out = ui_recti_from_mu(clip);
            return true;
        }
    }
    return false;
}

/* The colour text `s` was drawn in, as 0xRRGGBB. */
static inline bool
ui_probe_text_rgb(const char* s, uint32_t* rgb) {
    const mu_TextCommand* text = ui_probe_text_command(s);
    if (text == NULL) {
        return false;
    }
    *rgb = ui_rgb_value(text->color);
    return true;
}

/* The colour of the first rectangle filled exactly at `r`: a button's face
 * is its bezel's first span. */
static inline bool
ui_probe_fill_rgb(recti_t r, uint32_t* rgb) {
    mu_Command* cmd = NULL;
    while (mu_next_command(ui_context(), &cmd)) {
        const mu_Rect c = cmd->rect.rect;
        if (cmd->type == MU_COMMAND_RECT && c.x == r.x && c.y == r.y && c.w == r.w && c.h == r.h) {
            *rgb = ui_rgb_value(cmd->rect.color);
            return true;
        }
    }
    return false;
}

/* How wide a label may be inside a button, or a dropdown, `w` wide under
 * `theme`. */
static inline int
ui_probe_button_label_width(int w, bool has_icon, const ui_theme_t* theme) {
    const ui_mu_theme_t t = ui_theme_from_face(theme);
    return ui_icon_button_label_width(w, has_icon, &t);
}

static inline int
ui_probe_dropdown_label_width(int w, const ui_theme_t* theme) {
    const ui_mu_theme_t t = ui_theme_from_face(theme);
    return ui_dropdown_label_width(w, &t);
}

/* Where a dropdown at `anchor` opens its list of `count` rows `row_h` tall. */
static inline recti_t
ui_probe_dropdown_list_rect(recti_t anchor, int count, int row_h, int canvas_h) {
    return ui_recti_from_mu(ui_dropdown_list_rect(ui_mu_from_recti(anchor), count, row_h, canvas_h, UI_MARGIN));
}

/* The ink coverage text `s` was drawn at, 0-255; -1 when it was not drawn. */
static inline int
ui_probe_text_alpha(const char* s) {
    const mu_TextCommand* text = ui_probe_text_command(s);
    return text == NULL ? -1 : text->color.a;
}

/* The open dropdown list: its rows' area and how far they are scrolled.
 * False while no list is open. */
static inline bool
ui_probe_list(recti_t* rows, int* scroll_y) {
    const mu_Container* list = ui_dropdown_open_list();
    if (list == NULL) {
        return false;
    }
    *rows = ui_recti_from_mu(list->body);
    *scroll_y = list->scroll.y;
    return true;
}

#ifndef ESP_PLATFORM
#define UI_PROBE_WIDGETS  8
#define UI_PROBE_KEY_MAX  48
#define UI_PROBE_KEY_MARK "###"

typedef struct {
    char key[UI_PROBE_KEY_MAX];
    recti_t rect;
} ui_probe_widget_t;

static ui_probe_widget_t* ui_probe_widgets;
static int ui_probe_widget_count;

/* A widget's identity key: what follows "###", or else the whole label. */
static inline const char*
ui_probe_key(const char* label) {
    const char* mark = strstr(label, UI_PROBE_KEY_MARK);
    return mark != NULL ? mark + sizeof UI_PROBE_KEY_MARK - 1 : label;
}

static void
ui_probe_observe(const char* label, recti_t r) {
    const char* key = ui_probe_key(label);
    int i = 0;
    while (i < ui_probe_widget_count && strcmp(ui_probe_widgets[i].key, key) != 0) {
        i++;
    }
    if (i == UI_PROBE_WIDGETS) {
        TEST_FAIL_MESSAGE("more widgets than ui_probe.h records; raise UI_PROBE_WIDGETS");
    }
    if (i == ui_probe_widget_count) {
        if (strlen(key) >= sizeof ui_probe_widgets[i].key) {
            TEST_FAIL_MESSAGE("a widget key longer than ui_probe.h records; raise UI_PROBE_KEY_MAX");
        }
        ui_probe_widget_count++;
        (void)snprintf(ui_probe_widgets[i].key, sizeof ui_probe_widgets[i].key, "%s", key);
    }
    ui_probe_widgets[i].rect = r;
}

/* Records every face widget's latest rectangle by its identity key, until
 * ui_probe_widgets_stop(), which a suite's cleanup must call. */
static inline void
ui_probe_widgets_start(void) {
    ui_probe_widgets = memory_alloc(UI_PROBE_WIDGETS * sizeof *ui_probe_widgets, MEMORY_PSRAM);
    TEST_ASSERT_NOT_NULL(ui_probe_widgets);
    ui_probe_widget_count = 0;
    ui_face_widget_for_test = ui_probe_observe;
}

static inline void
ui_probe_widgets_stop(void) {
    ui_face_widget_for_test = NULL;
    memory_free(ui_probe_widgets);
    ui_probe_widgets = NULL;
    ui_probe_widget_count = 0;
}

/* The rectangle the widget keyed `key` was last drawn at. */
static inline bool
ui_probe_widget(const char* key, recti_t* out) {
    for (int i = 0; i < ui_probe_widget_count; i++) {
        if (strcmp(ui_probe_widgets[i].key, key) == 0) {
            *out = ui_probe_widgets[i].rect;
            return true;
        }
    }
    return false;
}
#endif
