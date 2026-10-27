#include "input/touch/gesture.h"

bool
gesture_in_edge_zone(vec2i_t point, gesture_edge_t edge, vec2i_t screen) {
    switch (edge) {
        case GESTURE_EDGE_TOP: return point.y <= GESTURE_HOME_ZONE_DEPTH;
        case GESTURE_EDGE_BOTTOM: return point.y >= screen.y - GESTURE_HOME_ZONE_DEPTH;
        case GESTURE_EDGE_LEFT: return point.x <= GESTURE_HOME_ZONE_DEPTH;
        case GESTURE_EDGE_RIGHT: return point.x >= screen.x - GESTURE_HOME_ZONE_DEPTH;
    }
    return false;
}

bool
gesture_is_home_swipe(const input_t* input, gesture_edge_t edge, int screen_w, int screen_h) {
    if (!input->down || !gesture_in_edge_zone(input->press, edge, (vec2i_t){screen_w, screen_h})) {
        return false;
    }

    /* "Travelled" is measured toward the centre (away from the edge) in
     * whichever sign that means for it, since y grows downward and x grows
     * rightward. */
    int travelled_toward_centre = 0;
    switch (edge) {
        case GESTURE_EDGE_TOP: travelled_toward_centre = input->point.y - input->press.y; break;
        case GESTURE_EDGE_BOTTOM: travelled_toward_centre = input->press.y - input->point.y; break;
        case GESTURE_EDGE_LEFT: travelled_toward_centre = input->point.x - input->press.x; break;
        case GESTURE_EDGE_RIGHT: travelled_toward_centre = input->press.x - input->point.x; break;
    }
    return travelled_toward_centre >= GESTURE_HOME_SWIPE_DIST;
}
