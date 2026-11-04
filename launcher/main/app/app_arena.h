/*
 * app_arena: the bulk memory the shell lends to whichever app is running.
 *
 * The app region: all of the PSRAM heap but the system reserve, taken once at
 * boot before any other PSRAM user and never given back, so no visit can
 * fragment the system heap and no system buffer can split the region. At each
 * app entry the region becomes one empty TLSF heap (MEMORY_APP in
 * core/memory.h); at exit it is dropped whole, so nothing a visit takes
 * outlives it and a leak cannot carry into the next visit.
 *
 * The arena bumps through chunks taken from that heap. An app never frees:
 * leaving is the free. A mark and a rewind to it scope a shorter lifetime
 * inside one visit. One take is one contiguous block, however large.
 *
 * Rejected: a size each app declares and is refused on start. The region is
 * whatever the system leaves, so an app that does not fit fails where it
 * allocates, as it would on a smaller board. Rejected too: a fixed block
 * linked into PSRAM, which leaves idle whatever the system does not use.
 *
 * Called from the frame loop only; not safe from another task or core.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "gfx/gfx.h"

/* PSRAM the system holds besides its two framebuffers, at its peak: 46.4 KB
 * at boot, 41 KB at the launcher (`autana heap psram`). */
#define APP_ARENA_SYSTEM_OTHER_BYTES   (48u * 1024u)

/* The PSRAM heap the region leaves the system: the framebuffer, one more
 * buffer its size, and the rest. The second is the double-framebuffer back
 * buffer the boot animation draws through, freed before the launcher, then
 * whichever the shell makes during a visit: a back buffer, the send audit's
 * shadow, a readback's snapshot, or the half picture a scene expands (a
 * quarter of the size). A development send audit opened over a half picture
 * does not fit and is refused with a log line. */
#define APP_ARENA_SYSTEM_RESERVE_BYTES ((2u * GFX_FRAMEBUFFER_BYTES) + APP_ARENA_SYSTEM_OTHER_BYTES)

/* The smallest chunk the arena takes from the app heap: small takes share one
 * heap block and its header, and an emptied tail is at most this much. A take
 * larger than it gets a chunk of its own size. */
#define APP_ARENA_CHUNK_BYTES          (16u * 1024u)

/* Takes the region from the PSRAM heap. Boot calls it once, before any other
 * PSRAM allocation can split the heap. False when the heap cannot give it;
 * every take then fails. */
bool app_arena_init(void);

/* The region's size: the PSRAM heap less APP_ARENA_SYSTEM_RESERVE_BYTES, or 0
 * before app_arena_init() or when it failed. */
size_t app_arena_bytes(void);

/* The shell calls these around each visit: enter makes the region an empty
 * app heap before the app's enter(); exit rewinds the arena and drops the
 * heap after its exit(), whatever the app left in it. */
void app_arena_enter(void);
void app_arena_exit(void);

/* NULL when `size` is 0, `align` is not a power of two, or the app heap cannot
 * hold it; a refusal takes nothing. Contents are undefined. */
void* app_arena_take(size_t size, size_t align);

/* Where the arena stands, to rewind to: 0 on an empty arena. It counts the
 * tails left in chunks a larger take moved past, so it is a position, not
 * the bytes in use. */
size_t app_arena_mark(void);

/* Gives back everything taken since `mark`. Marks nest: rewind to them in
 * the reverse order they were made. A mark past what is in use asserts. */
void app_arena_rewind(size_t mark);
