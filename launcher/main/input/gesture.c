#include "input/gesture.h"

bool
gesture_in_edge_zone(int x, int y, gesture_edge_t edge, int screen_w, int screen_h) {
    switch (edge) {
        case GESTURE_EDGE_TOP: return y <= GESTURE_HOME_ZONE_DEPTH;
        case GESTURE_EDGE_BOTTOM: return y >= screen_h - GESTURE_HOME_ZONE_DEPTH;
        case GESTURE_EDGE_LEFT: return x <= GESTURE_HOME_ZONE_DEPTH;
        case GESTURE_EDGE_RIGHT: return x >= screen_w - GESTURE_HOME_ZONE_DEPTH;
    }
    return false;
}

gesture_edge_t
gesture_opposite_edge(gesture_edge_t edge) {
    switch (edge) {
        case GESTURE_EDGE_TOP: return GESTURE_EDGE_BOTTOM;
        case GESTURE_EDGE_BOTTOM: return GESTURE_EDGE_TOP;
        case GESTURE_EDGE_LEFT: return GESTURE_EDGE_RIGHT;
        case GESTURE_EDGE_RIGHT: return GESTURE_EDGE_LEFT;
    }
    return edge;
}

bool
gesture_is_edge_swipe(const input_t* input, gesture_edge_t edge, int screen_w, int screen_h) {
    if (!input->down || !gesture_in_edge_zone(input->press_x, input->press_y, edge, screen_w, screen_h)) {
        return false;
    }

    /* "Travelled" is measured toward the centre (away from the edge) in
     * whichever sign that means for it, since y grows downward and x grows
     * rightward. */
    int travelled_toward_centre = 0;
    switch (edge) {
        case GESTURE_EDGE_TOP: travelled_toward_centre = input->y - input->press_y; break;
        case GESTURE_EDGE_BOTTOM: travelled_toward_centre = input->press_y - input->y; break;
        case GESTURE_EDGE_LEFT: travelled_toward_centre = input->x - input->press_x; break;
        case GESTURE_EDGE_RIGHT: travelled_toward_centre = input->press_x - input->x; break;
    }
    return travelled_toward_centre >= GESTURE_HOME_SWIPE_DIST;
}
