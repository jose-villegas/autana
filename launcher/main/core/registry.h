/* Internal engine registration lists; not part of the app contract.
 * Identity and name clashes leave all links untouched. */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <string.h>

typedef struct {
    size_t link, name;
    int (*names)(const char* a, const char* b);
    int (*order)(const void* a, const void* b);
} registry_rule_t;

#define REGISTRY_RULE(T, names, order) {offsetof(T, next), offsetof(T, name), (names), (order)}

typedef enum { REGISTRY_ADDED, REGISTRY_ALREADY, REGISTRY_CLASH } registry_add_t;

static inline void*
registry_next(const void* entry, size_t link) {
    void* next;
    memcpy(&next, (const unsigned char*)entry + link, sizeof next);
    return next;
}

static inline bool
registry_contains(const void* head, size_t link, const void* entry) {
    for (; head != NULL && head != entry; head = registry_next(head, link)) {}
    return head != NULL;
}

static inline registry_add_t
registry_add(void* head, const registry_rule_t* rule, void* entry) {
    const char* name = registry_next(entry, rule->name);
    for (void* at = registry_next(head, 0); at != NULL; at = registry_next(at, rule->link)) {
        if (at == entry || rule->names(registry_next(at, rule->name), name) == 0) {
            return at == entry ? REGISTRY_ALREADY : REGISTRY_CLASH;
        }
    }
    void* at;
    for (at = registry_next(head, 0);
         at != NULL && (rule->order ? rule->order(at, entry) : rule->names(registry_next(at, rule->name), name)) < 0;
         at = registry_next(at, rule->link)) {
        head = (unsigned char*)at + rule->link;
    }
    memcpy((unsigned char*)entry + rule->link, &at, sizeof at);
    memcpy(head, &entry, sizeof entry);
    return REGISTRY_ADDED;
}
