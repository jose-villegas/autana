#include "render_lab_mode_switch.h"

void
render_lab_mode_switch_request(render_lab_mode_switch_t* state) {
    state->pending = true;
}

bool
render_lab_mode_switch_take(render_lab_mode_switch_t* state) {
    const bool pending = state->pending;
    state->pending = false;
    return pending;
}

bool
render_lab_mode_switch_double_buffer_allowed(bool band_mode_active, bool scene_is_cube) {
    return !band_mode_active && scene_is_cube;
}
