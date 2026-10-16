/* ui_face: plain-value UI verbs. */
#include "ui/ui.h"
#include "ui/ui_internal.h"

#ifndef ESP_PLATFORM
void (*ui_scope_imbalance_for_test)(void);
#endif

void
ui_theme(const ui_theme_t* theme) {
    (void)theme;
}

void
ui_screen(void) {}

void
ui_window(recti_t r) {
    (void)r;
}

void
ui_row(int h) {
    (void)h;
}

recti_t
ui_next(void) {
    return (recti_t){0};
}

bool
ui_button(recti_t r, const char* label) {
    (void)r;
    (void)label;
    return false;
}

bool
ui_tile(recti_t r, const char* label, bool selected) {
    (void)r;
    (void)label;
    (void)selected;
    return false;
}

bool
ui_toggle(recti_t r, const char* label, bool* on) {
    (void)r;
    (void)label;
    (void)on;
    return false;
}

bool
ui_slider(recti_t r, const char* label, int* value, int lo, int hi) {
    (void)r;
    (void)label;
    (void)value;
    (void)lo;
    (void)hi;
    return false;
}

bool
ui_dropdown(recti_t r, const char* label, int* index, const char* const* names, int count) {
    (void)r;
    (void)label;
    (void)index;
    (void)names;
    (void)count;
    return false;
}

bool
ui_dropdown_icons(recti_t r, const char* label, int* index, const char* const* names, const draw_image_t* const* icons,
                  int count) {
    (void)icons;
    return ui_dropdown(r, label, index, names, count);
}

void
ui_text(recti_t r, const char* s) {
    (void)r;
    (void)s;
}

void
ui_text_right(recti_t r, const char* s) {
    ui_text(r, s);
}

void
ui_text_centre(recti_t r, const char* s) {
    ui_text(r, s);
}

void
ui_caption(recti_t r, const char* s) {
    ui_text(r, s);
}

void
ui_header(recti_t r, const char* title) {
    ui_text(r, title);
}

void
ui_panel(recti_t r) {
    (void)r;
}

void
ui_fill(recti_t r, uint32_t rgb) {
    (void)r;
    (void)rgb;
}

void
ui_outline(recti_t r, uint32_t rgb) {
    (void)r;
    (void)rgb;
}

void
ui_icon(recti_t r, const draw_image_t* icon) {
    (void)r;
    (void)icon;
}

void
ui_swatches(recti_t r, const uint32_t* rgb, int cols, int rows) {
    (void)r;
    (void)rgb;
    (void)cols;
    (void)rows;
}

void
ui_disabled_begin(bool disabled) {
    (void)disabled;
}

void
ui_disabled_end(void) {}

void
ui_color_begin(ui_color_slot_t slot, uint32_t rgb) {
    (void)slot;
    (void)rgb;
}

void
ui_color_end(void) {}

void
ui_scale_begin(int scale) {
    (void)scale;
}

void
ui_scale_end(void) {}
