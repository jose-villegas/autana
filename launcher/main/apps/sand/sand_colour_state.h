/*
 * sand_colour_state - which gfx pixel-format transition each sand app event
 * requires, as a standalone, ESP-IDF-free module so a host suite can prove
 * the one invariant that matters without gfx.c or a device: indexed mode
 * never survives a path back to the title screen, because neither menu
 * screen has an indexed draw path; either would touch a framebuffer that
 * does not exist.
 *
 * app_sand.c owns the actual gfx_mode_enter()/exit() calls; this only
 * decides WHEN one is needed and tracks whether indexed mode is currently
 * active or merely suspended (behind the palette/brush screen). A grant can
 * fail - sand_colour_grant_failed() rolls the optimistic state back rather
 * than a caller re-deriving it.
 */
#pragma once

#include <stdbool.h>

typedef enum {
    SAND_COLOUR_FULL,
    SAND_COLOUR_256,
    SAND_COLOUR_16,
    SAND_COLOUR_MODE_COUNT,
} sand_colour_mode_t;

typedef enum {
    SAND_GFX_NONE,          /* no gfx_mode_enter()/exit() call needed */
    SAND_GFX_ENTER_INDEXED, /* call gfx_mode_enter() with GFX_PIXFMT_INDEXED8 */
    SAND_GFX_EXIT_TO_FULL,  /* call gfx_mode_exit() */
} sand_gfx_action_t;

typedef struct {
    bool indexed_active;
    bool indexed_suspended;
} sand_colour_state_t;

static inline void
sand_colour_state_init(sand_colour_state_t* st) {
    st->indexed_active = false;
    st->indexed_suspended = false;
}

/* start_sim(): `requested` is the committed COLOR MODE option. */
static inline sand_gfx_action_t
sand_colour_on_start_sim(sand_colour_state_t* st, sand_colour_mode_t requested) {
    st->indexed_suspended = false;
    if (requested == SAND_COLOUR_FULL) {
        st->indexed_active = false;
        return SAND_GFX_NONE;
    }
    st->indexed_active = true; /* optimistic - see sand_colour_grant_failed() */
    return SAND_GFX_ENTER_INDEXED;
}

/* gfx_mode_enter() returned FULL_FB instead of the indexed layout asked
 * for - the request just made was never granted, so nothing needs undoing
 * on the gfx side; only the state that assumed it would be does. */
static inline void
sand_colour_grant_failed(sand_colour_state_t* st) {
    st->indexed_active = false;
}

/* Every path back to the title screen: sand_enter(), plus a defensive call
 * right before the menu itself draws. Idempotent - safe to call when already FULL. */
static inline sand_gfx_action_t
sand_colour_on_enter_menu(sand_colour_state_t* st) {
    st->indexed_suspended = false;
    if (st->indexed_active) {
        st->indexed_active = false;
        return SAND_GFX_EXIT_TO_FULL;
    }
    return SAND_GFX_NONE;
}

/* Leaving the app entirely (home swipe) - the same requirement as reaching
 * the menu: whatever ran next assumes GFX_LAYOUT_FULL_FB/RGB565. */
static inline sand_gfx_action_t
sand_colour_on_exit_app(sand_colour_state_t* st) {
    return sand_colour_on_enter_menu(st);
}

/* The palette or brush screen is opening - neither draws through the
 * indexed pipeline yet, so indexed mode is suspended (not simply cleared,
 * unlike the menu paths above): sand_colour_on_close_overlay() restores it. */
static inline sand_gfx_action_t
sand_colour_on_open_overlay(sand_colour_state_t* st) {
    if (st->indexed_active) {
        st->indexed_active = false;
        st->indexed_suspended = true;
        return SAND_GFX_EXIT_TO_FULL;
    }
    return SAND_GFX_NONE;
}

static inline sand_gfx_action_t
sand_colour_on_close_overlay(sand_colour_state_t* st) {
    if (st->indexed_suspended) {
        st->indexed_suspended = false;
        st->indexed_active = true; /* optimistic - see sand_colour_grant_failed() */
        return SAND_GFX_ENTER_INDEXED;
    }
    return SAND_GFX_NONE;
}

static inline bool
sand_colour_indexed_active(const sand_colour_state_t* st) {
    return st->indexed_active;
}
