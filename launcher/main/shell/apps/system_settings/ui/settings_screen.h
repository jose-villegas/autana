/* The settings list: sections of owner-backed toggle rows. */
#pragma once
#include <stdbool.h>

/* One row: its label and its owner's getter and setter. */
typedef struct {
    const char* label;
    bool (*get)(void);
    void (*set)(bool on);
} settings_toggle_t;

typedef struct {
    const char* heading;
    const settings_toggle_t* rows;
    int row_count;
} settings_section_t;

/* Reads every row from its owner and calls the setter of a row the frame
 * changed. Caller brackets drawing with ui_begin()/ui_end(). */
void settings_screen_draw(const settings_section_t* sections, int section_count);
