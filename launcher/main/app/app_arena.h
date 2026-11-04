/* app_arena: nested bump lifetimes over chunks from the visit's TLSF heap.
 * The region is taken once at boot, never returned to the system heap, and
 * recreated empty at each app entry. Called from the frame loop only. */
#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "gfx/gfx.h"

/* PSRAM the system holds besides the framebuffer, measured at the launcher
 * with `autana heap psram`. */
#define APP_ARENA_SYSTEM_OTHER_BYTES   (41u * 1024u)

/* The PSRAM heap the region leaves the system: the framebuffer, one more
 * buffer its size that the shell makes on demand during a visit (the
 * double-framebuffer back buffer, the send audit's shadow, a readback's
 * snapshot), and the rest. */
#define APP_ARENA_SYSTEM_RESERVE_BYTES (2u * GFX_FRAMEBUFFER_BYTES + APP_ARENA_SYSTEM_OTHER_BYTES)

/* Amortizes heap metadata for small takes without keeping a large idle tail. */
#define APP_ARENA_CHUNK_BYTES          (16u * 1024u)

void app_arena_enter(void);
void app_arena_exit(void);

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
