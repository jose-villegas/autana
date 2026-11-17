#include "core/tune.h"

/* A release build has no tunables: the file is compiled for it, since the
 * source list is not conditional, and comes to nothing. */
#if TUNE_ENABLED

#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include "core/checkpoint.h"
#include "core/registry.h"

void
tune_checkpoint_capture(checkpoint_t* saved, const tune_registry_t* registry) {
    for (const tune_entry_t* e = registry->first; e != NULL; e = e->next) {
        checkpoint_capture(saved, e->value, sizeof(*e->value));
        checkpoint_capture(saved, e->owner, sizeof(*e->owner));
    }
}

static tune_registry_t shared;

tune_registry_t*
tune_shared(void) {
    return &shared;
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
tune_reset(tune_registry_t* registry, const char* name) {
    const tune_entry_t* entry = tune_find(registry, name);
    return entry != NULL && tune_set(registry, name, entry->initial);
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

#else

typedef int tune_has_nothing_in_a_release_build_t;

#endif
