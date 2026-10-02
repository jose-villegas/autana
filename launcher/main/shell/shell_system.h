/*
 * shell_system: the engine systems the frame loop runs every pass, in one
 * fixed order, without naming any of them. A system is a set of phase
 * callbacks; the loop calls each phase over every registered system, lowest
 * `order` first, and skips a system whose callback for that phase is NULL.
 *
 * A system below the shell is registered from a file in shell/, since a
 * layer only includes downward and the system's own folder cannot see this
 * header.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

/* Every system's place in a pass, all in one list, so a new system is
 * placed against the ones already here rather than guessed. Gaps leave room
 * to insert one between two others. */
#define SHELL_ORDER_SCENE 100

typedef struct shell_system {
    const char* name;
    int order; /* a SHELL_ORDER_* constant */

    /* While the last frame is still being sent: no framebuffer may be
     * touched. Runs only on a pass that overlaps the present. */
    void (*update)(uint32_t dt_ms);
    /* Into the framebuffer, before the app's frame() draws over it. Runs only
     * on a pass that overlaps the present. */
    void (*compose)(uint32_t dt_ms);
    /* The running app has exited: free what was held on its behalf. */
    void (*app_exit)(void);
    /* True when the app's frame must be presented a pass late, so update()
     * has the send to overlap; NULL means never. */
    bool (*overlaps_present)(void);

    /* The registry's link, set by shell_system_register(). */
    struct shell_system* next;
} shell_system_t;

/* Inserts by `order`, then by name, so link order never shows. */
void shell_system_register(shell_system_t* system);

/* Registers a static shell_system_t before app_main(), like APP_REGISTER. */
#define SHELL_SYSTEM_REGISTER(symbol)                                                                                  \
    __attribute__((constructor)) static void symbol##_register(void) { shell_system_register(&symbol); }

void shell_systems_update(uint32_t dt_ms);
void shell_systems_compose(uint32_t dt_ms);
void shell_systems_app_exit(void);
bool shell_systems_overlap_present(void);

/* For tests: makes `head` the list and returns the one it replaced, so a
 * test registers its own systems and puts the real ones back. Unguarded, as
 * the shell names no platform; the firmware never calls it. */
shell_system_t* shell_system_swap_for_test(shell_system_t* head);
