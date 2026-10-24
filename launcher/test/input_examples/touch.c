/* Touch: count taps, follow the finger. Call from your app's update(dt_ms, input). */
#include "input/input.h"

typedef struct {
    int taps;
    int x, y;
} touch_example_t;

void
touch_example(touch_example_t* s, const input_t* input) {
    if (input->pressed) {
        s->taps++; /* an edge: once per touch, however long it is held */
    }
    if (input->down) {
        s->x = input->x; /* the level: every frame the finger stays down */
        s->y = input->y;
    }
}
