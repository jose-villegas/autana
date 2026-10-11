/*
 * registry: sorted intrusive lists for engine registration, with identity
 * and clash checks before any link changes. Internal to the engine; this
 * helper is not part of the app contract.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <string.h>

typedef struct {
    size_t link;
    int (*order)(const void* a, const void* b);
    bool (*clash)(const void* a, const void* b);
} registry_rule_t;

typedef enum { REGISTRY_ADDED, REGISTRY_ALREADY, REGISTRY_CLASH } registry_add_t;

static inline bool
registry_contains(const void* head, size_t link, const void* entry) {
    for (; head != NULL; memcpy(&head, (const unsigned char*)head + link, sizeof head)) {
        if (head == entry) {
            return true;
        }
    }
    return false;
}

static inline registry_add_t
registry_add(void* head, const registry_rule_t* rule, void* entry) {
    void* current;
    memcpy(&current, head, sizeof current);
    if (registry_contains(current, rule->link, entry)) {
        return REGISTRY_ALREADY;
    }
    void* insertion = NULL;
    void* slot = head;
    while (current != NULL) {
        const int order = rule->order(current, entry);
        if (rule->clash ? rule->clash(current, entry) : order == 0) {
            return REGISTRY_CLASH;
        }
        if (insertion == NULL && order >= 0) {
            insertion = slot;
        }
        slot = (unsigned char*)current + rule->link;
        memcpy(&current, slot, sizeof current);
    }
    if (insertion == NULL) {
        insertion = slot;
    }
    memcpy(&current, insertion, sizeof current);
    memcpy((unsigned char*)entry + rule->link, &current, sizeof current);
    memcpy(insertion, &entry, sizeof entry);
    return REGISTRY_ADDED;
}
