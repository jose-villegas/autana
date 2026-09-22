/* console_navigation_parse - portable app-name matching for OPEN. */
#pragma once

#include <ctype.h>
#include <stdbool.h>

static inline bool
console_app_name_matches(const char* name, const char* prefix) {
    if (*prefix == '\0') {
        return false;
    }
    while (*prefix != '\0') {
        if (tolower((unsigned char)*name) != tolower((unsigned char)*prefix)) {
            return false;
        }
        name++;
        prefix++;
    }
    return true;
}
