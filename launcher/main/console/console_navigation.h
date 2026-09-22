/* console_navigation - development console requests that the shell applies
 * at a frame boundary. */
#pragma once

#include <stdbool.h>
#include <stddef.h>

typedef enum {
    CONSOLE_NAVIGATION_APPS,
    CONSOLE_NAVIGATION_OPEN,
    CONSOLE_NAVIGATION_HOME,
} console_navigation_t;

bool console_navigation_take_request(console_navigation_t* navigation, char* name, size_t name_size);
