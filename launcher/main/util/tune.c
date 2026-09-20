#include "util/tune.h"

/* A release build has no tunables: the file is compiled for it, since the
 * source list is not conditional, and comes to nothing. */
#if TUNE_ENABLED

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define REPLY_MAX 112

static tune_registry_t shared;

tune_registry_t*
tune_shared(void) {
    return &shared;
}

bool
tune_handle_line(const char* line, tune_reply_fn reply) {
    return tune_registry_handle_line(&shared, line, reply);
}

static bool
listed_in(const tune_entry_t* list, const tune_entry_t* entry) {
    for (; list != NULL; list = list->next) {
        if (list == entry) {
            return true;
        }
    }
    return false;
}

const tune_entry_t*
tune_find(const tune_registry_t* registry, const char* name) {
    for (const tune_entry_t* entry = registry->first; entry != NULL; entry = entry->next) {
        if (strcmp(entry->name, name) == 0) {
            return entry;
        }
    }
    return NULL;
}

bool
tune_register(tune_registry_t* registry, tune_entry_t* entry) {
    if (listed_in(registry->first, entry)) {
        return true;
    }
    if (listed_in(registry->clashed, entry)) {
        return false;
    }
    if (tune_find(registry, entry->name) != NULL) {
        entry->next = registry->clashed;
        registry->clashed = entry;
        return false;
    }
    tune_entry_t** link = &registry->first;
    while (*link != NULL && strcmp((*link)->name, entry->name) < 0) {
        link = &(*link)->next;
    }
    entry->next = *link;
    *link = entry;
    return true;
}

int
tune_count(const tune_registry_t* registry) {
    int count = 0;
    for (const tune_entry_t* entry = registry->first; entry != NULL; entry = entry->next) {
        count++;
    }
    return count;
}

static void
reply_value(tune_reply_fn reply, const tune_entry_t* entry) {
    char line[REPLY_MAX];
    snprintf(line, sizeof line, "TUNE_OK %s=%ld", entry->name, (long)*entry->value);
    reply(line);
}

static void
reply_error(tune_reply_fn reply, const char* reason, const char* about) {
    char line[REPLY_MAX];
    snprintf(line, sizeof line, "TUNE_ERR %s %s", reason, about);
    reply(line);
}

static void
list_all(const tune_registry_t* registry, tune_reply_fn reply) {
    char line[REPLY_MAX];
    for (const tune_entry_t* entry = registry->first; entry != NULL; entry = entry->next) {
        snprintf(line, sizeof line, "TUNE %s=%ld min=%ld max=%ld default=%ld", entry->name, (long)*entry->value,
                 (long)entry->low, (long)entry->high, (long)entry->initial);
        reply(line);
    }
    for (const tune_entry_t* entry = registry->clashed; entry != NULL; entry = entry->next) {
        reply_error(reply, "clash", entry->name);
    }
    snprintf(line, sizeof line, "TUNE_END count=%d", tune_count(registry));
    reply(line);
}

static void
change(const tune_entry_t* entry, int32_t value, tune_reply_fn reply) {
    *entry->value = value;
    if (entry->owner != NULL) {
        entry->owner->generation++;
    }
    reply_value(reply, entry);
}

/* `rest` is what follows "SET ": a name, one space, a whole number. */
static void
set_from(const tune_registry_t* registry, const char* rest, tune_reply_fn reply) {
    char name[TUNE_NAME_MAX + 1];
    const char* space = strchr(rest, ' ');
    if (space == NULL || space == rest || (size_t)(space - rest) > TUNE_NAME_MAX) {
        reply_error(reply, "usage", "SET <name> <value>");
        return;
    }
    memcpy(name, rest, (size_t)(space - rest));
    name[space - rest] = '\0';

    const tune_entry_t* entry = tune_find(registry, name);
    if (entry == NULL) {
        reply_error(reply, "unknown", name);
        return;
    }
    char* end = NULL;
    const long value = strtol(space + 1, &end, 0);
    if (end == space + 1 || *end != '\0') {
        reply_error(reply, "not-a-number", space + 1);
        return;
    }
    if (value < entry->low || value > entry->high) {
        char range[TUNE_NAME_MAX + sizeof " takes -2147483648..-2147483648"];
        snprintf(range, sizeof range, "%s takes %ld..%ld", name, (long)entry->low, (long)entry->high);
        reply_error(reply, "range", range);
        return;
    }
    change(entry, (int32_t)value, reply);
}

/* A name alone after its verb: what GET and RESET take. */
static const tune_entry_t*
named(const tune_registry_t* registry, const char* name, tune_reply_fn reply) {
    const tune_entry_t* entry = tune_find(registry, name);
    if (entry == NULL) {
        reply_error(reply, "unknown", name);
    }
    return entry;
}

bool
tune_registry_handle_line(tune_registry_t* registry, const char* line, tune_reply_fn reply) {
    if (strcmp(line, "TUNE") == 0) {
        list_all(registry, reply);
        return true;
    }
    if (strncmp(line, "GET ", 4) == 0) {
        const tune_entry_t* entry = named(registry, line + 4, reply);
        if (entry != NULL) {
            reply_value(reply, entry);
        }
        return true;
    }
    if (strncmp(line, "RESET ", 6) == 0) {
        const tune_entry_t* entry = named(registry, line + 6, reply);
        if (entry != NULL) {
            change(entry, entry->initial, reply);
        }
        return true;
    }
    if (strncmp(line, "SET ", 4) == 0) {
        set_from(registry, line + 4, reply);
        return true;
    }
    return false;
}

#else

typedef int tune_has_nothing_in_a_release_build_t;

#endif
