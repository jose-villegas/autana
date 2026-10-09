/*
 * gfx_debug: development-build instruments on the present path, overlays
 * drawn over what is sent and counts of how it was sent. Declared only
 * under CONFIG_LAUNCHER_DEVELOPMENT so calling one from a non-development
 * file fails to compile rather than silently no-opping.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "util/build/build_variant.h"

/* Runtime toggle for the panel-grid overlay layer: outlines whichever grid
 * cells are actually sent each frame, cyan for a full-row send and yellow
 * for a gathered run. Off by default even in a development build: it
 * draws over real content, so it should be opted into, not always on.
 * Independent of the leaf layer below. */
#if CONFIG_LAUNCHER_DEVELOPMENT
void gfx_set_debug_overlay(bool on);
bool gfx_debug_overlay(void);

/* A second, fully independent overlay layer, not a refinement of the one
 * above: outlines the leaves gfx_dirty.h's dirty_mark() actually marked
 * dirty this frame, in green, the leaves that were really touched, not
 * the static leaf lattice. Leaf bits are only ever set by a caller that
 * hands dirty_mark() a real box (see mark_leaves()); mark_band() never
 * marks leaves, so a region only touched that way legitimately shows
 * nothing here; that is a consequence of the design, not a bug. */
void gfx_set_leaf_overlay(bool on);
bool gfx_debug_leaf_overlay(void);

/* Runtime toggle for the send audit: a PSRAM shadow of every pixel handed
 * to the panel, compared with the framebuffer after each present, logging
 * pixels the panel was never sent and pixels read back wrong from PSRAM.
 * Off by default: it costs a full-screen compare per present. */
void gfx_set_send_audit(bool on);
bool gfx_send_audit(void);
/* Pixels the panel was never sent, counted since the audit was last turned on. */
int64_t gfx_send_audit_uncovered_px(void);

/* Per-strip counts of which send path the last stretch of gfx_present()
 * calls actually took (full-band, a gathered send of runs, or a
 * full-width send at less than the whole band's height) for a device
 * test to log alongside its own timing rather than guessing the split
 * from the number alone. Reset explicitly, not by gfx_present() itself,
 * so a caller can accumulate across exactly the frames it is measuring. */
void gfx_reset_strip_send_counts(void);
void gfx_get_strip_send_counts(int* full_bands, int* gathered, int* partial_bands);

/* Panel-format bytes queued since the last gfx_reset_strip_send_counts(),
 * every send path alike, so a device test can compare pixel formats that
 * have no strip/gather distinction of their own (GFX_LAYOUT_INDEXED)
 * against ones that do. */
int64_t gfx_get_bytes_sent(void);

/* The part of gfx_get_bytes_sent() that heal strips added. */
int64_t gfx_get_heal_bytes_sent(void);

/* Test-only: every strip of the framebuffer sent as a full band, bypassing
 * every dirty-tracking decision gfx_present() makes, the bus-time side of
 * a full present. */
void gfx_present_raw_full_frame_for_test(void);
#endif
