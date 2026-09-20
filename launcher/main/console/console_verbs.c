#include "console/console_verbs.h"

#include <string.h>

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
        if (strcmp(entry->name, verb->name) == 0) {
            return false;
        }
    }
    console_verb_t** link = &registry->first;
    while (*link != NULL && strcmp((*link)->name, verb->name) < 0) {
        link = &(*link)->next;
    }
    verb->next = *link;
    *link = verb;
    return true;
}

bool
console_registry_handle_line(console_registry_t* registry, const char* line, console_reply_fn reply) {
    for (const console_verb_t* entry = registry->first; entry != NULL; entry = entry->next) {
        const size_t n = strlen(entry->name);
        if (strncmp(line, entry->name, n) != 0) {
            continue;
        }
        if (line[n] == '\0') {
            entry->handle("", reply);
            return true;
        }
        if (line[n] == ' ') {
            entry->handle(line + n + 1, reply);
            return true;
        }
    }
    return false;
}
