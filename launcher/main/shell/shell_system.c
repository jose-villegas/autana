/* Portable system registration keeps firmware and host phases in the same run order. */
#include "shell/shell_system.h"

#include <assert.h>
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

void
shell_system_register(shell_system_t* system) {
    static const registry_rule_t rule = REGISTRY_RULE(shell_system_t, strcmp, runs_before);
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
