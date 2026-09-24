/*
 * sand_theme - the sand app's one UI palette: dark navy panels, a gold
 * face for the selected choice. The brush screen, the title screen and the
 * options screen all draw in it, so a choice looks the same on each.
 *
 * The values are defines, not only a ui_theme_t, because a host preview of
 * a screen must agree on these exact values rather than keep its own copy.
 * None derive from a material's colour, unlike the material palette.
 */
#pragma once

#include "ui/ui_widgets.h"

#define SAND_THEME_PANEL_FACE_COLOR  0x131C2E
#define SAND_THEME_PANEL_EDGE_COLOR  0xE8ECF4
#define SAND_THEME_SELECTED_COLOR    0xE0A63C
#define SAND_THEME_BUTTON_FACE_COLOR 0x1B2740
#define SAND_THEME_CAPTION_COLOR     0x8FA3C0
#define SAND_THEME_TEXT_COLOR        0xF2F6FF
#define SAND_THEME_ON_SELECTED_COLOR 0x2A1A06
#define SAND_THEME_MUTED_COLOR       0x4A5672

extern const ui_theme_t sand_ui_theme;
