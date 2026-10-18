/* What a face frame built, for an app's suite that drives a screen through
 * the face: where a widget landed, where a line of text was drawn, and
 * whether a dropdown's list is open. Read from ui/'s private state, so the
 * suite itself includes only ui.h. */
#pragma once

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

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
    const mu_Context* ctx = ui_context();
    *out = (recti_t){text->pos.x, text->pos.y, ctx->text_width(text->font, s, -1), ctx->text_height(text->font)};
    return true;
}

/* The ink coverage text `s` was drawn at, 0-255; -1 when it was not drawn. */
static inline int
ui_probe_text_alpha(const char* s) {
    const mu_TextCommand* text = ui_probe_text_command(s);
    return text == NULL ? -1 : text->color.a;
}

/* The open dropdown list: the window over the screen, its rows' area and
 * how far they are scrolled. False while no list is open. */
static inline bool
ui_probe_list(recti_t* rows, int* scroll_y) {
    const mu_Context* ctx = ui_context();
    for (int i = 1; i < ctx->root_list.idx; i++) {
        const mu_Container* cnt = ctx->root_list.items[i];
        if (cnt->open) {
            *rows = ui_recti_from_mu(cnt->body);
            *scroll_y = cnt->scroll.y;
            return true;
        }
    }
    return false;
}

#ifndef ESP_PLATFORM
#define UI_PROBE_WIDGETS   8
#define UI_PROBE_LABEL_MAX 48

static struct {
    char label[UI_PROBE_LABEL_MAX];
    recti_t rect;
} ui_probe_widgets[UI_PROBE_WIDGETS];

static int ui_probe_widget_count;

static void
ui_probe_observe(const char* label, recti_t r) {
    int i = 0;
    while (i < ui_probe_widget_count && strcmp(ui_probe_widgets[i].label, label) != 0) {
        i++;
    }
    if (i == UI_PROBE_WIDGETS) {
        TEST_FAIL_MESSAGE("more widgets than ui_probe.h records; raise UI_PROBE_WIDGETS");
    }
    if (i == ui_probe_widget_count) {
        ui_probe_widget_count++;
        (void)snprintf(ui_probe_widgets[i].label, sizeof ui_probe_widgets[i].label, "%s", label);
    }
    ui_probe_widgets[i].rect = r;
}

/* Records every face widget's latest rectangle by its label, until
 * ui_probe_widgets_stop(). */
static inline void
ui_probe_widgets_start(void) {
    ui_probe_widget_count = 0;
    ui_face_widget_for_test = ui_probe_observe;
}

static inline void
ui_probe_widgets_stop(void) {
    ui_face_widget_for_test = NULL;
}

/* The rectangle the widget labelled `label` was last drawn at. */
static inline bool
ui_probe_widget(const char* label, recti_t* out) {
    for (int i = 0; i < ui_probe_widget_count; i++) {
        if (strcmp(ui_probe_widgets[i].label, label) == 0) {
            *out = ui_probe_widgets[i].rect;
            return true;
        }
    }
    return false;
}
#endif
