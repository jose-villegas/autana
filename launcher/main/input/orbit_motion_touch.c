#include "input/orbit_motion_touch.h"

#include <stdlib.h>

#pragma GCC diagnostic error "-Wdouble-promotion"

static gesture_edge_t
opposite(gesture_edge_t edge) {
    switch (edge) {
        case GESTURE_EDGE_TOP: return GESTURE_EDGE_BOTTOM;
        case GESTURE_EDGE_BOTTOM: return GESTURE_EDGE_TOP;
        case GESTURE_EDGE_LEFT: return GESTURE_EDGE_RIGHT;
        case GESTURE_EDGE_RIGHT: return GESTURE_EDGE_LEFT;
    }
    return edge;
}

/* Within GESTURE_HOME_ZONE_DEPTH of `edge`, as gesture.c's own start test. */
static bool
in_strip(int x, int y, gesture_edge_t edge, viewport_t viewport) {
    switch (edge) {
        case GESTURE_EDGE_TOP: return y <= GESTURE_HOME_ZONE_DEPTH;
        case GESTURE_EDGE_BOTTOM: return y >= viewport.height - GESTURE_HOME_ZONE_DEPTH;
        case GESTURE_EDGE_LEFT: return x <= GESTURE_HOME_ZONE_DEPTH;
        case GESTURE_EDGE_RIGHT: return x >= viewport.width - GESTURE_HOME_ZONE_DEPTH;
    }
    return false;
}

void
orbit_motion_touch_init(orbit_motion_touch_t* touch) {
    *touch = (orbit_motion_touch_t){0};
}

/* The finger's move since last frame, turned into radians the way the
 * picture is held: right turns the near side of the model right (the eye
 * goes left), down tilts its top toward the eye (the eye goes up). */
static void
steer(orbit_motion_touch_t* touch, const input_t* input, viewport_t viewport, orbit_motion_input_t* out) {
    int ux0, uy0, ux1, uy1;
    viewport_physical_to_upright(viewport, touch->last_x, touch->last_y, &ux0, &uy0);
    viewport_physical_to_upright(viewport, input->x, input->y, &ux1, &uy1);
    const int shorter = viewport.width < viewport.height ? viewport.width : viewport.height;
    const float per_px = ORBIT_MOTION_TOUCH_TURN_PER_SHORT_SIDE / (float)shorter;
    out->yaw_turn = -(float)(ux1 - ux0) * per_px;
    out->pitch_turn = (float)(uy1 - uy0) * per_px;
    out->held = true;
    touch->last_x = input->x;
    touch->last_y = input->y;
    touch->strayed = touch->strayed || abs(input->x - input->press_x) > ORBIT_MOTION_TOUCH_TAP_SLOP_PX
                     || abs(input->y - input->press_y) > ORBIT_MOTION_TOUCH_TAP_SLOP_PX;
}

/* A lift that was a tap arms a double tap, or completes one; any other lift
 * disarms it. */
static void
lift(orbit_motion_touch_t* touch, orbit_motion_input_t* out) {
    const bool tap = !touch->strayed && touch->down_ms <= ORBIT_MOTION_TOUCH_TAP_MAX_MS;
    out->reset = tap && touch->tapped;
    touch->tapped = tap && !touch->tapped;
    touch->since_tap_ms = 0;
    touch->steering = false;
}

orbit_motion_input_t
orbit_motion_touch_step(orbit_motion_touch_t* touch, const input_t* input, uint32_t dt_ms, viewport_t viewport,
                        gesture_edge_t home_edge) {
    orbit_motion_input_t out = {0};
    if (touch->tapped) {
        touch->since_tap_ms += dt_ms;
        touch->tapped = touch->since_tap_ms <= ORBIT_MOTION_TOUCH_DOUBLE_TAP_MS;
    }
    if (input->pressed) {
        touch->steering = !in_strip(input->press_x, input->press_y, home_edge, viewport)
                          && !in_strip(input->press_x, input->press_y, opposite(home_edge), viewport);
        touch->last_x = input->press_x;
        touch->last_y = input->press_y;
        touch->down_ms = 0;
        touch->strayed = false;
    }
    if (!touch->steering) {
        return out;
    }
    if (input->down) {
        touch->down_ms += dt_ms;
        steer(touch, input, viewport, &out);
    } else if (input->released) {
        lift(touch, &out);
    }
    return out;
}
