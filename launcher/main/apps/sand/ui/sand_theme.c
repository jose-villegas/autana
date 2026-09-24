#include "sand_theme.h"

const ui_theme_t sand_ui_theme = {
    .panel_face = UI_RGB(SAND_THEME_PANEL_FACE_COLOR),
    .panel_edge = UI_RGB(SAND_THEME_PANEL_EDGE_COLOR),
    .button_face = UI_RGB(SAND_THEME_BUTTON_FACE_COLOR),
    .accent = UI_RGB(SAND_THEME_SELECTED_COLOR),
    .accent_face = UI_RGB(SAND_THEME_SELECTED_COLOR),
    .on_accent = UI_RGB(SAND_THEME_ON_SELECTED_COLOR),
    .text = UI_RGB(SAND_THEME_TEXT_COLOR),
    .caption = UI_RGB(SAND_THEME_CAPTION_COLOR),
    .muted = UI_RGB(SAND_THEME_MUTED_COLOR),
    .text_scale = 2,
    .icon_side = 32,
};
