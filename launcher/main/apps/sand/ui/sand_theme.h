/* The app's one palette keeps controls consistent across pages. Defines also
 * let host previews use the same values without maintaining another palette. */
#pragma once

#include "ui/ui_mu.h"

#define SAND_THEME_PANEL_FACE_COLOR  0x131C2E
#define SAND_THEME_PANEL_EDGE_COLOR  0xE8ECF4
#define SAND_THEME_SELECTED_COLOR    0xE0A63C
#define SAND_THEME_BUTTON_FACE_COLOR 0x1B2740
#define SAND_THEME_CAPTION_COLOR     0x8FA3C0
#define SAND_THEME_TEXT_COLOR        0xF2F6FF
#define SAND_THEME_ON_SELECTED_COLOR 0x2A1A06
#define SAND_THEME_MUTED_COLOR       0x4A5672

extern const ui_mu_theme_t sand_ui_theme;
