/*
 * ui_anchor - fixed-point anchors and pivots for pure rectangle placement.
 */
#pragma once

#include "ui/ui_transform.h"

typedef struct {
    ui_fp_t x, y;
} ui_anchor_t;

#define UI_ANCHOR_TOP_LEFT     ((ui_anchor_t){0, 0})
#define UI_ANCHOR_TOP          ((ui_anchor_t){UI_FP_ONE / 2, 0})
#define UI_ANCHOR_TOP_RIGHT    ((ui_anchor_t){UI_FP_ONE, 0})
#define UI_ANCHOR_LEFT         ((ui_anchor_t){0, UI_FP_ONE / 2})
#define UI_ANCHOR_CENTER       ((ui_anchor_t){UI_FP_ONE / 2, UI_FP_ONE / 2})
#define UI_ANCHOR_RIGHT        ((ui_anchor_t){UI_FP_ONE, UI_FP_ONE / 2})
#define UI_ANCHOR_BOTTOM_LEFT  ((ui_anchor_t){0, UI_FP_ONE})
#define UI_ANCHOR_BOTTOM       ((ui_anchor_t){UI_FP_ONE / 2, UI_FP_ONE})
#define UI_ANCHOR_BOTTOM_RIGHT ((ui_anchor_t){UI_FP_ONE, UI_FP_ONE})

static inline mu_Rect
ui_anchor_rect(mu_Rect parent, ui_anchor_t anchor, ui_anchor_t pivot, int offset_x, int offset_y, int w, int h) {
    const int x = parent.x + (int)(((int64_t)parent.w * anchor.x - (int64_t)w * pivot.x) / UI_FP_ONE) + offset_x;
    const int y = parent.y + (int)(((int64_t)parent.h * anchor.y - (int64_t)h * pivot.y) / UI_FP_ONE) + offset_y;
    return (mu_Rect){x, y, w, h};
}

static inline mu_Rect
ui_rect_inset(mu_Rect rect, int left, int top, int right, int bottom) {
    return (mu_Rect){rect.x + left, rect.y + top, rect.w - left - right, rect.h - top - bottom};
}
