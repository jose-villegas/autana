/*
 * app_arena - the one block of bulk memory the shell lends to whichever app
 * is running.
 *
 * A static block in PSRAM, handed out by bumping an offset. The shell empties
 * it before every app's enter(), so nothing an app takes outlives its visit
 * and an app never frees: leaving is the free. A mark and its release scope a
 * shorter lifetime inside one visit, such as one of several scenes.
 *
 * Rejected: per-app heap_caps_malloc()/free(). Each call site is one more
 * dynamic allocation to justify, a missed free() leaks across visits, and a
 * fragmented PSRAM heap can refuse a re-entry that worked the first time.
 *
 * Called from the frame loop only; not safe from another task or core.
 */
#pragma once

#include <stddef.h>

/* Half of the board's 8 MiB: over twice the largest working set an app has
 * asked for (about 1.7 MB), leaving the heap the rest for the framebuffer
 * (322 KiB) and every other PSRAM allocation. */
#define APP_ARENA_BYTES (4u * 1024u * 1024u)

typedef struct {
    size_t used;
    unsigned visit;
} app_arena_mark_t;

/* NULL when `size` is 0, `align` is not a power of two, or the rest of the
 * block cannot hold it; a refusal takes nothing. Contents are undefined. */
void* app_arena_take(size_t size, size_t align);

app_arena_mark_t app_arena_mark(void);

/* Gives back everything taken since `mark`. Marks nest: release them in the
 * reverse order they were made. One from before a reset does nothing. */
void app_arena_release(app_arena_mark_t mark);

size_t app_arena_used(void);

/* The shell's, before each app's enter(); an app never calls it. */
void app_arena_reset(void);
