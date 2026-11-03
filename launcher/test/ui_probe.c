#include "ui_probe.h"

#include <stdio.h>
#include <string.h>

#include "core/memory.h"
#include "ui/ui_internal.h"
#include "unity.h"

static const mu_TextCommand*
text_command(const char* s) {
    mu_Command* cmd = NULL;
    while (mu_next_command(ui_context(), &cmd)) {
        if (cmd->type == MU_COMMAND_TEXT && strcmp(cmd->text.str, s) == 0) {
            return &cmd->text;
        }
    }
    return NULL;
}

void
ui_probe_end_frame(void) {
    ui_end_commands();
}

bool
ui_probe_text(const char* s, recti_t* out) {
    const mu_TextCommand* text = text_command(s);
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

bool
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

bool
ui_probe_text_rgb(const char* s, uint32_t* rgb) {
    const mu_TextCommand* text = text_command(s);
    if (text == NULL) {
        return false;
    }
    *rgb = ui_rgb_value(text->color);
    return true;
}

/* The colour of the grid's cell that fills exactly `r` once clipped to `clip`. */
static bool
swatch_cell_rgb(const ui_swatch_command_t* grid, recti_t clip, recti_t r, uint32_t* rgb) {
    for (int row = 0; row < grid->rows; row++) {
        for (int col = 0; col < grid->cols; col++) {
            const recti_t cell = recti_intersect(ui_recti_from_mu(ui_swatch_cell_rect(grid, col, row)), clip);
            if (memcmp(&cell, &r, sizeof r) == 0) {
                *rgb = grid->rgb[row * grid->cols + col];
                return true;
            }
        }
    }
    return false;
}

bool
ui_probe_fill_rgb(recti_t r, uint32_t* rgb) {
    mu_Command* cmd = NULL;
    recti_t clip = {0, 0, ui_width(), ui_height()};
    while (mu_next_command(ui_context(), &cmd)) {
        if (cmd->type == MU_COMMAND_CLIP) {
            clip = ui_recti_from_mu(cmd->clip.rect);
        } else if (cmd->type == MU_COMMAND_RECT && memcmp(&cmd->rect.rect, &r, sizeof r) == 0) {
            *rgb = ui_rgb_value(cmd->rect.color);
            return true;
        } else if (cmd->type == UI_COMMAND_SWATCHES && swatch_cell_rgb((const ui_swatch_command_t*)cmd, clip, r, rgb)) {
            return true;
        }
    }
    return false;
}

bool
ui_probe_icon_rgb(recti_t within, uint32_t* rgb) {
    mu_Command* cmd = NULL;
    while (mu_next_command(ui_context(), &cmd)) {
        if (cmd->type != MU_COMMAND_ICON && cmd->type != UI_COMMAND_IMAGE) {
            continue;
        }
        const ui_image_command_t* image = (const ui_image_command_t*)cmd;
        const recti_t r = ui_recti_from_mu(cmd->type == UI_COMMAND_IMAGE ? image->rect : cmd->icon.rect);
        if (r.x >= within.x && r.y >= within.y && recti_right(r) <= recti_right(within)
            && r.y + r.h <= within.y + within.h) {
            *rgb = ui_rgb_value(cmd->type == UI_COMMAND_IMAGE ? image->color : cmd->icon.color);
            return true;
        }
    }
    return false;
}

int
ui_probe_bevel_index(recti_t r, uint32_t face_rgb, bool sunken) {
    ui_span_t spans[UI_BEZEL_MAX_SPANS];
    const int n = ui_bezel_spans(ui_mu_from_recti(r), ui_rgb(face_rgb), sunken, spans, UI_BEZEL_MAX_SPANS);
    int last = -1;
    for (int i = 1; i < n; i++) {
        int found = -1;
        int index = 0;
        mu_Command* cmd = NULL;
        while (mu_next_command(ui_context(), &cmd)) {
            const mu_Rect c = cmd->rect.rect;
            if (cmd->type == MU_COMMAND_RECT && c.x == spans[i].rect.x && c.y == spans[i].rect.y
                && c.w == spans[i].rect.w && c.h == spans[i].rect.h
                && memcmp(&cmd->rect.color, &spans[i].color, sizeof spans[i].color) == 0) {
                found = index;
            }
            index++;
        }
        if (found < 0) {
            return -1;
        }
        last = found > last ? found : last;
    }
    return last;
}

/* `theme` as the face holds it, or the shell look for NULL, as ui_theme() does. */
static ui_mu_theme_t
probe_theme(const ui_theme_t* theme) {
    if (theme == NULL) {
        return ui_theme_from_style(ui_context()->style);
    }
    const ui_mu_theme_t t = UI_THEME_FROM_FACE(theme);
    return t;
}

int
ui_probe_button_label_width(int w, bool has_icon, const ui_theme_t* theme) {
    const ui_mu_theme_t t = probe_theme(theme);
    return ui_icon_button_label_width(w, has_icon, &t);
}

int
ui_probe_dropdown_label_width(int w, bool has_icons, const ui_theme_t* theme) {
    const ui_mu_theme_t t = probe_theme(theme);
    return ui_dropdown_label_width(w, has_icons, &t);
}

recti_t
ui_probe_dropdown_list_rect(recti_t anchor, int count, int row_h, int canvas_h) {
    return ui_recti_from_mu(ui_dropdown_list_rect(ui_mu_from_recti(anchor), count, row_h, canvas_h, UI_MARGIN));
}

int
ui_probe_dropdown_list_scroll(int selected, int count, int row_h, int list_h) {
    return ui_dropdown_list_scroll(selected, count, row_h, list_h);
}

int
ui_probe_text_alpha(const char* s) {
    const mu_TextCommand* text = text_command(s);
    return text == NULL ? -1 : text->color.a;
}

bool
ui_probe_list(recti_t* rows, int* scroll_y) {
    const mu_Container* list = ui_dropdown_open_list();
    if (list == NULL) {
        return false;
    }
    *rows = ui_recti_from_mu(list->body);
    *scroll_y = list->scroll.y;
    return true;
}

/* Host checks command growth; the device enforces 8 KiB. Reserve room for a fifth screen. */
#define UI_PROBE_HEADROOM_BYTES 2048
#define UI_PROBE_BUDGET         (MU_COMMANDLIST_SIZE - UI_PROBE_HEADROOM_BYTES)

void
ui_probe_budget_assert(const char* screen_name) {
    ui_end_commands();
    const int used = ui_context()->command_list.idx;
    char msg[96];
    snprintf(msg, sizeof msg, "%s screen used %d of %d budget bytes (%d headroom)", screen_name, used, UI_PROBE_BUDGET,
             MU_COMMANDLIST_SIZE - used);
    printf("%s\n", msg);
    TEST_ASSERT_LESS_THAN_INT_MESSAGE(UI_PROBE_BUDGET, used, msg);
}

#ifndef ESP_PLATFORM
#define UI_PROBE_WIDGETS  32
#define UI_PROBE_KEY_MAX  48
#define UI_PROBE_KEY_MARK "###"

typedef struct {
    char key[UI_PROBE_KEY_MAX];
    recti_t rect;
} ui_probe_widget_t;

static ui_probe_widget_t* ui_probe_widgets;
static int ui_probe_widget_count;

/* A widget's identity key: what follows "###", or else the whole label. */
static const char*
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

void
ui_probe_widgets_start(void) {
    ui_probe_widgets = memory_alloc(UI_PROBE_WIDGETS * sizeof *ui_probe_widgets, MEMORY_PSRAM);
    TEST_ASSERT_NOT_NULL(ui_probe_widgets);
    ui_probe_widget_count = 0;
    ui_face_widget_for_test = ui_probe_observe;
}

void
ui_probe_widgets_stop(void) {
    ui_face_widget_for_test = NULL;
    memory_free(ui_probe_widgets);
    ui_probe_widgets = NULL;
    ui_probe_widget_count = 0;
}

bool
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
