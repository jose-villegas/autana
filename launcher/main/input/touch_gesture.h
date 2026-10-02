/*
 * touch_gesture: the gesture an injected touch finishes as, and the console's
 * word for it. Pure, so the host tests it; touch.h is the driver that
 * reports it.
 */
#pragma once

typedef enum {
    TOUCH_GESTURE_NONE,
    TOUCH_GESTURE_TAP,
    TOUCH_GESTURE_PRESS,
    TOUCH_GESTURE_DRAG,
} touch_gesture_completion_t;

/* "TAP", "PRESS" or "DRAG"; NULL for none. */
const char* touch_gesture_name(touch_gesture_completion_t completion);
