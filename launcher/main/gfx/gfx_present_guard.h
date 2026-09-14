/*
 * gfx_present_guard - the "no present in flight" invariant every gfx_*
 * entry point that reads or writes drawing state or the framebuffer checks,
 * as a standalone, ESP-IDF-free module.
 *
 * Header-only and static, the same reason gfx_dirty.h is: gfx.c includes
 * this once and gets its own flag and trip counter; a host suite includes it
 * again and gets an independent copy to drive and inspect directly, with no
 * ESP-IDF dependency to satisfy - see suite_gfx_present_guard.c.
 *
 * Compiled out of a release device build entirely (GFX_PRESENT_GUARD() folds
 * to nothing there, so the check costs nothing), active on a development
 * device build and unconditionally on a host build: a release image never
 * runs update()/frame() overlapped with anything, so it has nothing to
 * assert against, while a host build has no device-build config at all and
 * is exactly where this contract needs proving.
 */
#pragma once

#include <stdbool.h>

#if defined(ESP_PLATFORM)
#include <assert.h>
#endif

static bool gfx_present_guard_in_flight;

#if !defined(ESP_PLATFORM) || CONFIG_LAUNCHER_DEVELOPMENT
static unsigned gfx_present_guard_trips;
#endif

/* gfx_present_begin() calls this once it has committed to sending - device
 * or host alike. */
static inline void
gfx_present_guard_begin(void) {
    gfx_present_guard_in_flight = true;
}

/* gfx_present_wait() calls this once sending is known complete. */
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
    if (!gfx_present_guard_in_flight) {
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
