/* What a face frame built, for an app's suite that drives a screen through
 * the face: where a widget landed, where a line of text was drawn, and
 * whether a dropdown's list is open. Plain values only; ui_probe.c reads
 * ui/'s private state, so a suite includes nothing of the backend. */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "ui/ui.h"

/* Closes the frame's command list without painting it, for a suite that
 * reads what the frame built. */
void ui_probe_end_frame(void);

/* Where a line of text `s` was drawn in the last built frame, in canvas
 * coordinates: its first glyph's corner and its measured size. */
bool ui_probe_text(const char* s, recti_t* out);

/* The clip rectangle in force when text `s` was drawn. */
bool ui_probe_text_clip(const char* s, recti_t* out);

/* The colour text `s` was drawn in, as 0xRRGGBB. */
bool ui_probe_text_rgb(const char* s, uint32_t* rgb);

/* The ink coverage text `s` was drawn at, 0-255; -1 when it was not drawn. */
int ui_probe_text_alpha(const char* s);

/* The colour of the first rectangle filled exactly at `r`: a button's face
 * is its bezel's first span. */
bool ui_probe_fill_rgb(recti_t r, uint32_t* rgb);

/* The colour of the first icon (a check, a chevron) drawn wholly inside `within`. */
bool ui_probe_icon_rgb(recti_t within, uint32_t* rgb);

/* Where in the command list a bevel of `face_rgb` on `r` was drawn: the
 * position of its last edge, or -1 unless every one of its edges (and
 * their colours) was drawn. A sunken bevel and a raised one share rects and
 * differ only in which edge pair is lit, so this tells them apart. */
int ui_probe_bevel_index(recti_t r, uint32_t face_rgb, bool sunken);

/* How wide a label may be inside a button, or a dropdown, `w` wide under
 * `theme`. */
int ui_probe_button_label_width(int w, bool has_icon, const ui_theme_t* theme);
int ui_probe_dropdown_label_width(int w, const ui_theme_t* theme);

/* Where a dropdown at `anchor` opens its list of `count` rows `row_h` tall. */
recti_t ui_probe_dropdown_list_rect(recti_t anchor, int count, int row_h, int canvas_h);

/* Where a list of `count` rows `row_h` tall, `list_h` high, opens scrolled
 * to with `selected` chosen. */
int ui_probe_dropdown_list_scroll(int selected, int count, int row_h, int list_h);

/* The open dropdown list: its rows' area and how far they are scrolled.
 * False while no list is open. */
bool ui_probe_list(recti_t* rows, int* scroll_y);

/* Closes the frame's command list and fails the test unless it fits the
 * device's list with headroom left for one more screen; `screen_name` names
 * it in the report line. */
void ui_probe_budget_assert(const char* screen_name);

#ifndef ESP_PLATFORM
/* Records every face widget's latest rectangle by its identity key, until
 * ui_probe_widgets_stop(), which a suite's cleanup must call. */
void ui_probe_widgets_start(void);
void ui_probe_widgets_stop(void);

/* The rectangle the widget keyed `key` was last drawn at. */
bool ui_probe_widget(const char* key, recti_t* out);
#endif
