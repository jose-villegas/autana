/* Charge one stage's time to a name. */
#include "profile/frame_cost.h"

void draw_the_thing(void);

void
frame_cost_example(void) {
    FRAME_COST_BEGIN(began);
    draw_the_thing();
    FRAME_COST_END(began, "thing.draw");
}
