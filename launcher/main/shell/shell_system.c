/* shell_system: registered engine systems linked by shell_system_t.next in run
 * order; no hardware dependency, so host tests run the board's phases. */
#include "shell/shell_system.h"

#include <assert.h>
#include <stddef.h>
#include <string.h>

#include "core/registry.h"

static shell_system_t* systems_head;

static int
run_order(const void* a, const void* b) {
    const shell_system_t* first = a;
    const shell_system_t* second = b;
    return first->order == second->order ? strcmp(first->name, second->name)
                                         : (first->order > second->order) - (first->order < second->order);
}

void
shell_system_register(shell_system_t* system) {
    static const registry_rule_t rule = REGISTRY_RULE(shell_system_t, strcmp, run_order);
    const registry_add_t result = registry_add(&systems_head, &rule, system);
    assert(result == REGISTRY_ADDED && "system registered twice or name taken");
    (void)result;
}

typedef void (*phase_fn_t)(uint32_t dt_ms);

/* Runs the timed phase stored at `offset` in each system, lowest order first. */
static void
run_phase(size_t offset, uint32_t dt_ms) {
    for (const shell_system_t* system = systems_head; system != NULL; system = system->next) {
        const phase_fn_t phase = *(const phase_fn_t*)((const char*)system + offset);
        if (phase != NULL) {
            phase(dt_ms);
        }
    }
}

void
shell_systems_poll(uint32_t dt_ms) {
    run_phase(offsetof(shell_system_t, poll), dt_ms);
}

void
shell_systems_update(uint32_t dt_ms) {
    run_phase(offsetof(shell_system_t, update), dt_ms);
}

void
shell_systems_compose(uint32_t dt_ms) {
    run_phase(offsetof(shell_system_t, compose), dt_ms);
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
