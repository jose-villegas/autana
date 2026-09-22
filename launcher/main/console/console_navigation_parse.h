/* console_navigation_parse - portable app-name matching for OPEN. */
#pragma once

#include <ctype.h>
#include <stdbool.h>

static inline bool
console_app_name_starts_with(const char* name, const char* prefix) {
    while (*prefix != '\0') {
        if (*name == '\0') {
            return false;
        }
        if (tolower((unsigned char)*name) != tolower((unsigned char)*prefix)) {
            return false;
        }
        name++;
        prefix++;
    }
    return true;
}

/* Any word of the name, not only the first: an app called "Falling Sand"
 * answers to the word a person reaches for. */
static inline bool
console_app_name_matches(const char* name, const char* prefix) {
    if (*prefix == '\0') {
        return false;
    }
    for (const char* at = name; *at != '\0'; at++) {
        const bool word_start = (at == name) || (at[-1] == ' ');
        if (word_start && console_app_name_starts_with(at, prefix)) {
            return true;
        }
    }
    return false;
}
