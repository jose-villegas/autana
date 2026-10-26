/* Gravity: call from the frame thread's app update. */
#include <math.h>
#include "input/input.h"

#define BALL_SPEED 0.1F /* pixels per ms at full strength */

typedef struct {
    float ball_x, ball_y, strength, shake;
} gravity_shared_example_t;

void
gravity_shared_example(gravity_shared_example_t* s, uint32_t dt_ms) {
    const vec3f_t down = input_gravity();
    s->strength = hypotf(down.x, down.y);
    s->shake = input_shake();
    s->ball_x += down.x * BALL_SPEED * dt_ms;
    s->ball_y += down.y * BALL_SPEED * dt_ms;
}
