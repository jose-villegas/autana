/* Touch: count taps, follow the finger. Call from your app's update(dt_ms, input). */
#include "input/input.h"

typedef struct {
    int taps;
    vec2i_t point;
} touch_example_t;

void
touch_example(touch_example_t* s, const input_t* input) {
    if (input->pressed) {
        s->taps++; /* an edge: once per touch, however long it is held */
    }
    if (input->down) {
        s->point = input->point; /* the level: every frame the finger stays down */
    }
}
