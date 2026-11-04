/*
 * app_arena: the one block of bulk memory the shell lends to whichever app
 * is running.
 *
 * The app region: all of the PSRAM heap but what the system keeps, taken once
 * at boot before any other PSRAM user and never given back, so no visit can
 * fragment it. Handed out by bumping an offset. The shell rewinds it to 0
 * right after every app's exit(), so nothing an app takes outlives its visit
 * and an app never frees: leaving is the free. A mark and a rewind to it scope
 * a shorter lifetime inside one visit.
 *
 * Rejected: per-app memory_alloc()/memory_free(). Each call site is one more
 * dynamic allocation to justify, a missed memory_free() leaks across visits,
 * and a fragmented PSRAM heap can refuse a re-entry that worked the first
 * time. Rejected too: a block each app declares and takes on entry, for that
 * same fragmentation, and a fixed block linked into PSRAM, which leaves idle
 * whatever the system does not use.
 *
 * Called from the frame loop only; not safe from another task or core.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "gfx/gfx.h"

/* PSRAM the system holds besides the framebuffer, measured at the launcher
 * with `autana heap psram` (autana-6t5u5). */
#define APP_ARENA_SYSTEM_OTHER_BYTES   (41u * 1024u)

/* The PSRAM heap the region leaves the system: the framebuffer, one more
 * buffer its size that the shell makes on demand during a visit (the
 * double-framebuffer back buffer, the send audit's shadow, a readback's
 * snapshot), and the rest. */
#define APP_ARENA_SYSTEM_RESERVE_BYTES (2u * GFX_FRAMEBUFFER_BYTES + APP_ARENA_SYSTEM_OTHER_BYTES)

/* Takes the region from the PSRAM heap. Boot calls it once, before any other
 * PSRAM allocation can split the heap. False when the heap cannot give it;
 * every take then fails. */
bool app_arena_init(void);

/* The region's size: the PSRAM heap less APP_ARENA_SYSTEM_RESERVE_BYTES, or 0
 * before app_arena_init() or when it failed. */
size_t app_arena_bytes(void);

/* NULL when `size` is 0, `align` is not a power of two, or the rest of the
 * block cannot hold it; a refusal takes nothing. Contents are undefined. */
void* app_arena_take(size_t size, size_t align);

/* How much is in use: 0 on an empty arena. */
size_t app_arena_mark(void);

/* Gives back everything taken since `mark`. Marks nest: rewind to them in
 * the reverse order they were made. A mark past what is in use asserts. */
void app_arena_rewind(size_t mark);
