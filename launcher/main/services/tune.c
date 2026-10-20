#include "services/tune.h"

/* A release build has no tunables: the file is compiled for it, since the
 * source list is not conditional, and comes to nothing. */
#if TUNE_ENABLED

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "core/registry.h"

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
    static const registry_rule_t rule = REGISTRY_RULE(tune_entry_t, strcmp, NULL);
    if (registry_contains(registry->clashed, rule.link, entry)) {
        return false;
    }
    if (registry_add(&registry->first, &rule, entry) == REGISTRY_CLASH) {
        entry->next = registry->clashed;
        registry->clashed = entry;
        return false;
    }
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
    (void)snprintf(line, sizeof line, "TUNE_OK %s=%ld", entry->name, (long)*entry->value);
    reply(line);
}

static void
reply_error(tune_reply_fn reply, const char* reason, const char* about) {
    char line[REPLY_MAX];
    (void)snprintf(line, sizeof line, "TUNE_ERR %s %s", reason, about);
    reply(line);
}

static void
list_all(const tune_registry_t* registry, tune_reply_fn reply) {
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

bool
tune_set(tune_registry_t* registry, const char* name, int32_t value) {
    const tune_entry_t* entry = tune_find(registry, name);
    if (entry == NULL || value < entry->low || value > entry->high) {
        return false;
    }
    *entry->value = value;
    if (entry->owner != NULL) {
        entry->owner->generation++;
    }
    return true;
}

bool
tune_parse_value(const char* text, int32_t* out_value) {
    char* end = NULL;
    errno = 0;
    const long value = strtol(text, &end, 0);
    if (errno != 0 || end == text || *end != '\0' || value < INT32_MIN || value > INT32_MAX) {
        return false;
    }
    *out_value = (int32_t)value;
    return true;
}

/* `rest` is what follows "SET ": a name, one space, a whole number. */
static void
set_from(tune_registry_t* registry, const char* rest, tune_reply_fn reply) {
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
            (void)tune_set(registry, entry->name, entry->initial);
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

typedef int tune_has_nothing_in_a_release_build_t;

#endif
