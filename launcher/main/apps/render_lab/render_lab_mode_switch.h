/* Deferred band-layout switch requested while the menu is drawing. */
#pragma once

#include <stdbool.h>

typedef struct {
    bool pending;
} render_lab_mode_switch_t;

void render_lab_mode_switch_request(render_lab_mode_switch_t* state);
bool render_lab_mode_switch_take(render_lab_mode_switch_t* state);
