#include "sand_theme.h"

const ui_theme_t sand_ui_theme = {
    .panel_face = UI_RGB(BRUSH_PANEL_FACE_COLOR),
    .panel_edge = UI_RGB(BRUSH_PANEL_BORDER_COLOR),
    .button_face = UI_RGB(BRUSH_SEG_UNSELECTED_COLOR),
    .accent = UI_RGB(BRUSH_SEG_SELECTED_COLOR),
    .accent_face = UI_RGB(BRUSH_SEG_SELECTED_COLOR),
    .on_accent = UI_RGB(BRUSH_SEG_SELECTED_INK_COLOR),
    .text = UI_RGB(BRUSH_TEXT_COLOR),
    .caption = UI_RGB(BRUSH_CAPTION_COLOR),
    .muted = UI_RGB(SAND_THEME_MUTED_COLOR),
    .text_scale = 2,
    .icon_side = 32,
};
