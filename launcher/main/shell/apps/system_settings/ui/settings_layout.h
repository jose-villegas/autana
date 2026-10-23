/* The settings list's fixed text; its rows flow at the toolkit's heights. */
#pragma once

#define SETTINGS_NOTE_LINE_COUNT 5

extern const char settings_display_heading[];
extern const char settings_developer_heading[];
extern const char settings_panel_clock_label[];
extern const char settings_overlay_label[];
extern const char settings_leaf_label[];
extern const char settings_interlace_label[];
extern const char settings_send_audit_label[];
/* Pre-broken to fit the portrait row: face text is one clipped line per rect. */
extern const char* const settings_panel_clock_note[SETTINGS_NOTE_LINE_COUNT];
