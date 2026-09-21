#include "console/console_verbs.h"

#include <string.h>

/* Verb names are matched ignoring case, so the lowercase name a person
 * types and the uppercase one a harness has always sent are the same
 * verb. ASCII by hand rather than strcasecmp(): this file is built for
 * the device and for a host test, and tolower()'s locale is a dependency
 * neither needs. */
static char
folded(char c) {
    return (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
}

static int
name_cmp(const char* a, const char* b) {
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

static bool
listed_in(const console_verb_t* list, const console_verb_t* verb) {
    for (; list != NULL; list = list->next) {
        if (list == verb) {
            return true;
        }
    }
    return false;
}

bool
console_register(console_registry_t* registry, console_verb_t* verb) {
    if (listed_in(registry->first, verb)) {
        return true;
    }
    for (const console_verb_t* entry = registry->first; entry != NULL; entry = entry->next) {
        if (name_cmp(entry->name, verb->name) == 0) {
            return false;
        }
    }
    console_verb_t** link = &registry->first;
    while (*link != NULL && name_cmp((*link)->name, verb->name) < 0) {
        link = &(*link)->next;
    }
    verb->next = *link;
    *link = verb;
    return true;
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

bool
console_registry_handle_line(console_registry_t* registry, const char* line, console_reply_fn reply) {
    for (const console_verb_t* entry = registry->first; entry != NULL; entry = entry->next) {
        const char* args;
        if (console_word_match(line, entry->name, &args)) {
            entry->handle(args, reply);
            return true;
        }
    }
    return false;
}

console_clash_t
console_find_clash(const console_registry_t* verbs, const char* const* prefixes, int n, const char** from,
                   const char** other) {
    for (int i = 0; i < n; i++) {
        if (strchr(prefixes[i], ' ') != NULL) {
            *from = prefixes[i];
            return CONSOLE_CLASH_SPACE;
        }
        if (strlen(prefixes[i]) + 1 >= CONSOLE_LINE_MAX) {
            *from = prefixes[i];
            return CONSOLE_CLASH_LENGTH;
        }
        for (const console_verb_t* v = verbs->first; v != NULL; v = v->next) {
            if (name_cmp(v->name, prefixes[i]) == 0) {
                *from = prefixes[i];
                *other = v->name;
                return CONSOLE_CLASH_VERB;
            }
        }
        for (int j = 0; j < i; j++) {
            if (name_cmp(prefixes[j], prefixes[i]) == 0) {
                *from = prefixes[i];
                *other = prefixes[j];
                return CONSOLE_CLASH_APP;
            }
        }
    }
    return CONSOLE_CLASH_NONE;
}
