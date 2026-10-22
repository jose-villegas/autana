/* The settings list's window, row heights and fixed text. */
#pragma once
#include "math/geometry/recti.h"

#define SETTINGS_NOTE_LINE_COUNT 5

typedef struct {
    recti_t window;
    int heading_height;
    int toggle_height;
    int note_height;
} settings_layout_t;

extern const char settings_display_heading[];
extern const char settings_developer_heading[];
extern const char settings_panel_clock_label[];
extern const char settings_overlay_label[];
extern const char settings_leaf_label[];
extern const char settings_interlace_label[];
extern const char settings_send_audit_label[];
/* Pre-broken to fit the portrait row: face text is one clipped line per rect. */
extern const char* const settings_panel_clock_note[SETTINGS_NOTE_LINE_COUNT];

settings_layout_t settings_layout(int width, int height, int text_height);
