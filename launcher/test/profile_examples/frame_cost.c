/* Bracket one drawing stage so a development build can charge its time. */
#include "profile/frame_cost.h"

typedef struct {
    int draws;
} frame_cost_example_t;

static void
draw_the_thing(frame_cost_example_t* s) {
    s->draws++;
}

static void
frame_cost_example(frame_cost_example_t* s) {
    FRAME_COST_BEGIN(began);
    draw_the_thing(s);
    FRAME_COST_END(began, "thing.draw");
}
