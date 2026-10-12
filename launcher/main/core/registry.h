/* Internal engine lists, not app-facing. Entries: T* next, const char* name.
 * name_cmp tests clashes and orders when order is NULL; order is a three-way compare. */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <string.h>

typedef struct {
    size_t link, name;
    int (*name_cmp)(const char* a, const char* b);
    int (*order)(const void* a, const void* b);
} registry_rule_t;

#define REGISTRY_RULE(T, name_cmp, order)                                                                              \
    {_Generic(((T*)0)->next, T*: offsetof(T, next)), _Generic(((T*)0)->name, const char*: offsetof(T, name)),          \
     (name_cmp), (order)}

typedef enum { REGISTRY_ADDED, REGISTRY_ALREADY, REGISTRY_CLASH } registry_add_t;

static inline void*
registry_load(const void* base, size_t offset) {
    void* next;
    memcpy(&next, (const unsigned char*)base + offset, sizeof next);
    return next;
}

static inline bool
registry_contains(const void* first, size_t link, const void* entry) {
    for (; first != NULL && first != entry; first = registry_load(first, link)) {}
    return first != NULL;
}

/* ALREADY if listed, CLASH if any name_cmp is 0: links untouched; else insert before first not ordered before entry. */
static inline registry_add_t
registry_add(void* head_slot, const registry_rule_t* rule, void* entry) {
    const char* name = registry_load(entry, rule->name);
    for (void* at = registry_load(head_slot, 0); at != NULL; at = registry_load(at, rule->link)) {
        if (at == entry) {
            return REGISTRY_ALREADY;
        }
        if (rule->name_cmp(registry_load(at, rule->name), name) == 0) {
            return REGISTRY_CLASH;
        }
    }
    void* at;
    for (at = registry_load(head_slot, 0);
         at != NULL && (rule->order ? rule->order(at, entry) : rule->name_cmp(registry_load(at, rule->name), name)) < 0;
         at = registry_load(at, rule->link)) {
        head_slot = (unsigned char*)at + rule->link;
    }
    memcpy((unsigned char*)entry + rule->link, &at, sizeof at);
    memcpy(head_slot, &entry, sizeof entry);
    return REGISTRY_ADDED;
}
