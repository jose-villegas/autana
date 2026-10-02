/*
 * shell_apps: switching between the launcher and the running app, and
 * stepping whichever is showing. `current` is NULL while a system screen,
 * the launcher or Control Center, shows.
 */
#pragma once

#include <stdint.h>

#include "app.h"
#include "input/gesture.h"
#include "input/input.h"

void shell_apps_init(void);
gesture_edge_t exit_edge_for_quarter(int quarter);
void start_app(const app_t** current, const app_t* next);
void exit_app(const app_t** current);
void leave_app(const app_t** current, input_t* input, gesture_edge_t exit_edge, uint32_t dt_ms);
void step_app(const app_t** current, input_t* input, uint32_t dt_ms);
void present_unless_deferred(const app_t* current);
