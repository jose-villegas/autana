/* console_navigation: APPS, OPEN and HOME, which the shell applies at a
 * frame boundary (shell.c) from console_navigation_slot. */
#pragma once

#include "console/console_frame_request.h"
#include "console/console_verbs.h"

typedef enum {
    CONSOLE_NAVIGATION_APPS,
    CONSOLE_NAVIGATION_OPEN,
    CONSOLE_NAVIGATION_HOME,
} console_navigation_t;

typedef struct {
    console_navigation_t navigation;
    char app[CONSOLE_LINE_MAX]; /* OPEN's name */
} console_navigation_request_t;

extern console_frame_slot_t console_navigation_slot;
