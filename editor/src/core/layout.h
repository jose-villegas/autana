#pragma once

#include <vector>

enum class LayoutOrientation {
    Portrait,
    Landscape,
};

struct LayoutRect {
    int x;
    int y;
    int width;
    int height;
};

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
