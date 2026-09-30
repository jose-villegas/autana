#pragma once
/*
 * Interface to heap_arena.c's bookkeeping, for timing.c's per-test hook.
 *
 * Everything here (and everything in heap_arena.c) is compiled in ONLY when
 * HOST_HEAP_ARENA is defined; see heap_arena.c's own top comment for why.
 * That means this header is safe to include unconditionally, but timing.c
 * still guards the #include itself: it costs nothing and keeps any other
 * build compiling timing.c from ever resolving this path at all.
 */
#ifdef HOST_HEAP_ARENA

#include <stddef.h>

/* Outstanding block count and byte total across the modeled internal and
 * PSRAM pools. */
void heap_arena_snapshot(size_t* out_blocks, size_t* out_bytes);

/* Highest outstanding-byte total observed since the last reset. Not zeroed
 * by the reset, floored at whatever is already outstanding, so one test's
 * peak is never reported lower than what a previous leak left behind (that
 * inflation is the point: it is what the device would also see). */
size_t heap_arena_peak_bytes(void);
void heap_arena_reset_peak(void);

/* The PSRAM heap a PSRAM size leaves: `override` as the environment variable
 * would carry it, or the device profile's size when NULL. */
size_t heap_arena_psram_heap_bytes(const char* override);

#endif /* HOST_HEAP_ARENA */
