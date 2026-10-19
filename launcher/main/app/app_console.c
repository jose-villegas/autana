#include "app/app_console.h"

#include <stdio.h>
#include <string.h>

void
console_reply_stdio(const char* line) {
    (void)printf("%s\n", line);
    (void)fflush(stdout);
}

/* ASCII by hand rather than strcasecmp(): this file is built for the device
 * and for a host test, and tolower()'s locale is a dependency neither needs. */
static char
folded(char c) {
    return (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
}

int
console_name_cmp(const char* a, const char* b) {
    while (*a != '\0' && folded(*a) == folded(*b)) {
        a++;
        b++;
    }
    return (int)(unsigned char)folded(*a) - (int)(unsigned char)folded(*b);
}

/* 0 if the first `n` characters of `line` are `name`, ignoring case. */
static int
name_ncmp(const char* line, const char* name, size_t n) {
    for (size_t i = 0; i < n; i++) {
        const int d = (int)(unsigned char)folded(line[i]) - (int)(unsigned char)folded(name[i]);
        if (d != 0 || line[i] == '\0') {
            return d;
        }
    }
    return 0;
}

bool
console_word_match(const char* line, const char* name, const char** args) {
    const size_t n = strlen(name);
    if (name_ncmp(line, name, n) != 0) {
        return false;
    }
    if (line[n] == '\0') {
        *args = line + n;
        return true;
    }
    if (line[n] == ' ') {
        *args = line + n + 1;
        return true;
    }
    return false;
}
