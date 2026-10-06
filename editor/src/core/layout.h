/* Geometry of authored screens using firmware rectangle storage. */
#pragma once

#include <vector>

#include "ui/control_center_layout_generated.h"

enum class LayoutOrientation {
    Portrait,
    Landscape,
};

using LayoutRect = control_center_layout_rect_t;

inline bool
operator==(const LayoutRect& first, const LayoutRect& second) {
    return first.x == second.x && first.y == second.y && first.width == second.width && first.height == second.height;
}

inline bool
operator!=(const LayoutRect& first, const LayoutRect& second) {
    return !(first == second);
}

// One orientation of a screen: rects[i] places the document's element i.
struct ScreenLayout {
    int canvas_width;
    int canvas_height;
    std::vector<LayoutRect> rects;
};

inline const char*
layout_orientation_id(LayoutOrientation orientation) {
    return orientation == LayoutOrientation::Landscape ? "landscape" : "portrait";
}
