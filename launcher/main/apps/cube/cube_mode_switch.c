#include "cube_mode_switch.h"

void
cube_mode_switch_request(cube_mode_switch_t* state) {
    state->pending = true;
}

bool
cube_mode_switch_take(cube_mode_switch_t* state) {
    const bool pending = state->pending;
    state->pending = false;
    return pending;
}
