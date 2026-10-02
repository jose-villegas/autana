/*
 * shell_system: the registered engine systems, one list threaded through
 * shell_system_t.next and kept in run order. Portable, so the host tests run
 * the same phases the board does.
 */
#include "shell/shell_system.h"

#include <assert.h>
#include <stddef.h>
#include <string.h>

static shell_system_t* systems_head;

static bool
runs_before(const shell_system_t* a, const shell_system_t* b) {
    return a->order < b->order || (a->order == b->order && strcmp(a->name, b->name) < 0);
}

void
shell_system_register(shell_system_t* system) {
    system->next = NULL;

    shell_system_t** link = &systems_head;
    while (*link != NULL && runs_before(*link, system)) {
        link = &(*link)->next;
    }
    assert((*link == NULL || strcmp((*link)->name, system->name) != 0) && "two systems share a name");
    system->next = *link;
    *link = system;
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
shell_systems_overlay(void) {
    for (const shell_system_t* system = systems_head; system != NULL; system = system->next) {
        if (system->overlay != NULL) {
            system->overlay();
        }
    }
}

void
shell_systems_invalidate(void) {
    for (const shell_system_t* system = systems_head; system != NULL; system = system->next) {
        if (system->invalidate != NULL) {
            system->invalidate();
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

#ifndef ESP_PLATFORM
shell_system_t*
shell_system_swap_for_test(shell_system_t* head) {
    shell_system_t* previous = systems_head;
    systems_head = head;
    return previous;
}
#endif
