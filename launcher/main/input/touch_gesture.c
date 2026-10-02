/* touch_gesture: touch_gesture.h's console words. */

#include "input/touch_gesture.h"

#include <stddef.h>

const char*
touch_gesture_name(touch_gesture_completion_t completion) {
    switch (completion) {
        case TOUCH_GESTURE_TAP: return "TAP";
        case TOUCH_GESTURE_PRESS: return "PRESS";
        case TOUCH_GESTURE_DRAG: return "DRAG";
        case TOUCH_GESTURE_NONE: break;
    }
    return NULL;
}
