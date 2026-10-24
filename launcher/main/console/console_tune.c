#include "console/console_tune.h"

/* A release build has no tunables, and so no lines for them. */
#if TUNE_ENABLED

#include <stdio.h>
#include <string.h>

#define REPLY_MAX 112

static void
reply_value(console_reply_fn reply, const tune_entry_t* entry) {
    char line[REPLY_MAX];
    (void)snprintf(line, sizeof line, "TUNE_OK %s=%ld", entry->name, (long)*entry->value);
    reply(line);
}

static void
reply_error(console_reply_fn reply, const char* reason, const char* about) {
    char line[REPLY_MAX];
    (void)snprintf(line, sizeof line, "TUNE_ERR %s %s", reason, about);
    reply(line);
}

static void
list_all(const tune_registry_t* registry, console_reply_fn reply) {
    char line[REPLY_MAX];
    for (const tune_entry_t* entry = registry->first; entry != NULL; entry = entry->next) {
        (void)snprintf(line, sizeof line, "TUNE %s=%ld min=%ld max=%ld default=%ld", entry->name, (long)*entry->value,
                       (long)entry->low, (long)entry->high, (long)entry->initial);
        reply(line);
    }
    for (const tune_entry_t* entry = registry->clashed; entry != NULL; entry = entry->next) {
        reply_error(reply, "clash", entry->name);
    }
    (void)snprintf(line, sizeof line, "TUNE_END count=%d", tune_count(registry));
    reply(line);
}

/* `rest` is what follows "SET ": a name, one space, a whole number. */
static void
set_from(tune_registry_t* registry, const char* rest, console_reply_fn reply) {
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
    int32_t value;
    if (!tune_parse_value(space + 1, &value)) {
        reply_error(reply, "not-a-number", space + 1);
        return;
    }
    if (!tune_set(registry, name, value)) {
        char range[TUNE_NAME_MAX + sizeof " takes -2147483648..-2147483648"];
        (void)snprintf(range, sizeof range, "%s takes %ld..%ld", name, (long)entry->low, (long)entry->high);
        reply_error(reply, "range", range);
        return;
    }
    reply_value(reply, entry);
}

/* A name alone after its verb: what GET and RESET take. */
static const tune_entry_t*
named(const tune_registry_t* registry, const char* name, console_reply_fn reply) {
    const tune_entry_t* entry = tune_find(registry, name);
    if (entry == NULL) {
        reply_error(reply, "unknown", name);
    }
    return entry;
}

bool
console_tune_line(tune_registry_t* registry, const char* line, console_reply_fn reply) {
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
            (void)tune_reset(registry, entry->name);
            reply_value(reply, entry);
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

typedef int console_tune_has_nothing_in_a_release_build_t;

#endif
