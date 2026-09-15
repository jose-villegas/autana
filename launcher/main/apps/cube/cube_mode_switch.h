/* Deferred band-layout switch requested while the menu is drawing. */
#pragma once

#include <stdbool.h>

typedef struct {
    bool pending;
} cube_mode_switch_t;

void cube_mode_switch_request(cube_mode_switch_t* state);
bool cube_mode_switch_take(cube_mode_switch_t* state);
