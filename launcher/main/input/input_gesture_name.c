/* input_gesture_name: the console's word for a finished gesture. */

#include "input/input_shell.h"

#include <stddef.h>

const char*
input_gesture_name(input_gesture_t gesture) {
    switch (gesture) {
        case INPUT_GESTURE_TAP: return "TAP";
        case INPUT_GESTURE_PRESS: return "PRESS";
        case INPUT_GESTURE_DRAG: return "DRAG";
        case INPUT_GESTURE_NONE: break;
    }
    return NULL;
}
