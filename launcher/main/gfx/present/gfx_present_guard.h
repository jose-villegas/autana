/*
 * gfx_present_guard: the "no present in flight" invariant every gfx_*
 * entry point that reads or writes drawing state or the framebuffer checks,
 * as a standalone, ESP-IDF-free module.
 *
 * Header-only over one instance (gfx_present.c), ESP-IDF-free so a host
 * suite drives it directly; see suite_gfx_present_guard.c.
 *
 * Compiled out of a release device build entirely (GFX_PRESENT_GUARD() folds
 * to nothing there, so the check costs nothing), active on a development
 * device build and unconditionally on a host build. Double-framebuffer
 * presentation leaves the guard idle: drawing writes the back buffer while
 * the send reads the front.
 */
#pragma once

#include <stdbool.h>

#include "build/build_variant.h"

#if defined(ESP_PLATFORM)
#include <assert.h>
#endif

extern bool gfx_present_guard_in_flight;
/* Set while a frame overlay replays during the send, which may draw. */
extern bool gfx_present_guard_replaying;

#if !defined(ESP_PLATFORM) || CONFIG_LAUNCHER_DEVELOPMENT
extern unsigned gfx_present_guard_trips;
#endif

/* Single-buffer present begins guard the shared draw/send state. */
static inline void
gfx_present_guard_begin(void) {
    gfx_present_guard_in_flight = true;
}

/* Ends the window in which drawing would touch the send source. */
static inline void
gfx_present_guard_end(void) {
    gfx_present_guard_in_flight = false;
}

/* A caller reaching this while a present is in flight broke the app
 * contract's rule that update() must not touch gfx (app.h): the framebuffer
 * or dirty tracker it is about to read or write may still be in use by the
 * present task. Loud on the device, where the culprit is a real bug worth
 * stopping for; a trip counter on a host build, since a fixture that
 * deliberately provokes this must keep running afterward to check it fired. */
static inline void
gfx_present_guard_check(void) {
    if (!gfx_present_guard_in_flight || gfx_present_guard_replaying) {
        return;
    }
#if !defined(ESP_PLATFORM) || CONFIG_LAUNCHER_DEVELOPMENT
    gfx_present_guard_trips++;
#endif
#if defined(ESP_PLATFORM)
    assert(false);
#endif
}

/* Folds to nothing in a release device build; active in development device
 * builds and on every host build. Call at the top of a public gfx_* entry
 * point that reads or writes drawing state or the framebuffer. */
#if !defined(ESP_PLATFORM) || CONFIG_LAUNCHER_DEVELOPMENT
#define GFX_PRESENT_GUARD() gfx_present_guard_check()
#else
#define GFX_PRESENT_GUARD() ((void)0)
#endif
