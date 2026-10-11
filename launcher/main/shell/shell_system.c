/*
 * shell_system: the registered engine systems, one list threaded through
 * shell_system_t.next and kept in run order. Portable, so the host tests run
 * the same phases the board does.
 */
#include "shell/shell_system.h"

#include <assert.h>
#include <stddef.h>
#include <string.h>

#include "core/registry.h"

static shell_system_t* systems_head;

static int
runs_before(const void* a, const void* b) {
    const shell_system_t* first = a;
    const shell_system_t* second = b;
    return first->order == second->order ? strcmp(first->name, second->name)
                                         : (first->order > second->order) - (first->order < second->order);
}

static bool
same_name(const void* a, const void* b) {
    const shell_system_t* first = a;
    const shell_system_t* second = b;
    return strcmp(first->name, second->name) == 0;
}

void
shell_system_register(shell_system_t* system) {
    static const registry_rule_t rule = {offsetof(shell_system_t, next), runs_before, same_name};
    const registry_add_t result = registry_add(&systems_head, &rule, system);
    assert(result == REGISTRY_ADDED && "two systems share a name");
    (void)result;
}

void
shell_systems_update(uint32_t dt_ms) {
    for (const shell_system_t* system = systems_head; system != NULL; system = system->next) {
        if (system->update != NULL) {
            system->update(dt_ms);
        }
    }
}

void
shell_systems_compose(uint32_t dt_ms) {
    for (const shell_system_t* system = systems_head; system != NULL; system = system->next) {
        if (system->compose != NULL) {
            system->compose(dt_ms);
        }
    }
}

void
shell_systems_app_exit(void) {
    for (const shell_system_t* system = systems_head; system != NULL; system = system->next) {
        if (system->app_exit != NULL) {
            system->app_exit();
        }
    }
}

bool
shell_systems_overlap_present(void) {
    for (const shell_system_t* system = systems_head; system != NULL; system = system->next) {
        if (system->overlaps_present != NULL && system->overlaps_present()) {
            return true;
        }
    }
    return false;
}

shell_system_t*
shell_system_swap_for_test(shell_system_t* head) {
    shell_system_t* previous = systems_head;
    systems_head = head;
    return previous;
}
