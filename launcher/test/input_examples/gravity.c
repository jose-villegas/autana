/* Gravity: a ball rolls downhill. Call from your app's update(dt_ms, input). */
#include "input/motion/gravity.h"
#include "input/input.h"

#define BALL_SPEED 0.1F /* pixels per ms at full tilt */

typedef struct {
    gravity_t gravity;
    input_motion_batch_t batch;
    float ball_x, ball_y;
} gravity_example_t;

void
gravity_example_start(gravity_example_t* s) {
    gravity_reset(&s->gravity);
}

void
gravity_example(gravity_example_t* s, uint32_t dt_ms) {
    if (input_read_motion_batch(&s->batch)) {
        gravity_update_batch(&s->gravity, &s->batch);
    }
    const vec3f_t down = gravity_down(&s->gravity);
    s->ball_x += down.x * BALL_SPEED * dt_ms;
    s->ball_y += down.y * BALL_SPEED * dt_ms;
}
