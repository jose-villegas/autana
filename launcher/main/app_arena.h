/*
 * app_arena - the one block of bulk memory the shell lends to whichever app
 * is running.
 *
 * A static block in PSRAM, handed out by bumping an offset. The shell rewinds
 * it to 0 right after every app's exit(), so nothing an app takes outlives
 * its visit and an app never frees: leaving is the free. A mark and a rewind to
 * it scope a shorter lifetime inside one visit.
 *
 * Rejected: per-app heap_caps_malloc()/free(). Each call site is one more
 * dynamic allocation to justify, a missed free() leaks across visits, and a
 * fragmented PSRAM heap can refuse a re-entry that worked the first time.
 *
 * Called from the frame loop only; not safe from another task or core.
 */
#pragma once

#include <stddef.h>

/* Half of the board's PSRAM, leaving the heap the rest for the framebuffer
 * and every other PSRAM allocation. */
#define APP_ARENA_BYTES (4u * 1024u * 1024u)

/* NULL when `size` is 0, `align` is not a power of two, or the rest of the
 * block cannot hold it; a refusal takes nothing. Contents are undefined. */
void* app_arena_take(size_t size, size_t align);

/* How much is in use: 0 on an empty arena. */
size_t app_arena_mark(void);

/* Gives back everything taken since `mark`. Marks nest: rewind to them in
 * the reverse order they were made. A mark past what is in use asserts. */
void app_arena_rewind(size_t mark);
