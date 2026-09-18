/*
 * gfx_fb_guard - the "there is a framebuffer to draw into" check every
 * gfx_* entry point that writes a pixel makes, as a standalone,
 * ESP-IDF-free module - the same reason gfx_present_guard.h is one.
 *
 * Unlike the present-in-flight guard, this one must run in every build,
 * release included: band mode (gfx_mode.h) frees the PSRAM framebuffer,
 * and a caller that still writes into it is a NULL-pointer write, not
 * merely a race worth catching during development. Only the LOUDNESS
 * differs by build - a development device build and a host build assert
 * or trip a counter; a release device build silently drops the draw,
 * which is what keeps a caller gfx cannot fix (a stray home-hint draw,
 * say) from crashing the board instead of losing one draw call.
 */
#pragma once

#include <stdbool.h>

#include "build_variant.h"

#if defined(ESP_PLATFORM)
#include <assert.h>
#endif

static bool gfx_fb_guard_available = true;

#if !defined(ESP_PLATFORM) || CONFIG_LAUNCHER_DEVELOPMENT
static unsigned gfx_fb_guard_trips;
#endif

/* gfx_mode_enter()/gfx_mode_exit() (gfx.c) call this as the framebuffer is
 * freed or restored. */
static inline void
gfx_fb_guard_set_available(bool available) {
    gfx_fb_guard_available = available;
}

/* True if it is safe to write a pixel. Callers MUST check this return
 * value and skip the write on false - nothing else stops a NULL write. */
static inline bool
gfx_fb_guard_ok(void) {
    if (gfx_fb_guard_available) {
        return true;
    }
#if !defined(ESP_PLATFORM) || CONFIG_LAUNCHER_DEVELOPMENT
    gfx_fb_guard_trips++;
#endif
#if defined(ESP_PLATFORM) && CONFIG_LAUNCHER_DEVELOPMENT
    assert(false);
#endif
    return false;
}

/* Call at the top of any gfx_* entry point that writes into the
 * framebuffer, and skip the write when it returns false. */
#define GFX_REQUIRE_FRAMEBUFFER() gfx_fb_guard_ok()
