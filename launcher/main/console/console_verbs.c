#include "console/console_verbs.h"

#include <string.h>

#include "core/registry.h"

bool
console_register(console_registry_t* registry, console_verb_t* verb) {
    static const registry_rule_t rule = REGISTRY_RULE(console_verb_t, console_name_cmp, NULL);
    return registry_add(&registry->first, &rule, verb) != REGISTRY_CLASH;
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
            if (console_name_cmp(v->name, prefixes[i]) == 0) {
                *from = prefixes[i];
                *other = v->name;
                return CONSOLE_CLASH_VERB;
            }
        }
        for (int j = 0; j < i; j++) {
            if (console_name_cmp(prefixes[j], prefixes[i]) == 0) {
                *from = prefixes[i];
                *other = prefixes[j];
                return CONSOLE_CLASH_APP;
            }
        }
    }
    return CONSOLE_CLASH_NONE;
}
