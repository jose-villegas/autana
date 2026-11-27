#pragma once

/* Where an inner loop sits within an instruction-cache line moves the frame
 * time. A pinned flash function starts on a line, whatever code links ahead
 * of it; `slots` two-byte nop.n, never executed, then sit ahead of its entry
 * so its loops land at the offsets that ran fastest. code_layout.py checks the
 * owner's code_layout.txt; measure and retune changed layouts on the board
 * before regenerating it. */

#ifdef ESP_PLATFORM
#include "esp_attr.h"
#include "sdkconfig.h"

#define CODE_ENTRY_OFFSET(slots)                                                                                       \
    __attribute__((aligned(CONFIG_ESP32S3_INSTRUCTION_CACHE_LINE_SIZE), patchable_function_entry(slots, slots)))
/* IRAM saves about a tenth of draw cycles in dense scenes and avoids
 * instruction cache misses in cull setup. Entries need 4-byte alignment.
 * IRAM budget: 7396 B (includes 4 B of Xtensa branch-target alignment padding) */
#define RENDER_IRAM_ATTR   IRAM_ATTR __attribute__((aligned(4)))
/* The draw path's copies for a view that skips clusters behind drawn depth
 * (render_view_t.skip_hidden), placed with the draw path. IRAM is granted per
 * measured ms saved per KB, the largest free internal block noted with it.
 * IRAM budget: release 4487 B, dev 4491 B, diag 4491 B */
#define RENDER_SKIP_ATTR   RENDER_IRAM_ATTR
/* The fill and row walk of a textured triangle that divides u and v by
 * depth, placed with the draw path as RENDER_SKIP_ATTR is: in IRAM they take
 * about a twentieth off the draw of near textured cards, all dividing.
 * IRAM budget: 1799 B */
#define RENDER_DIVIDE_ATTR RENDER_IRAM_ATTR
/* Flash code off the hot draw (a masked texture's fill): linker.lf places it
 * after all other flash text, so it never sits between the per-triangle
 * flash functions and moves their cache placement. */
#define RENDER_TAIL_ATTR   __attribute__((section(".render_tail.text")))
#else
/* A host build has no instruction cache to place code in. */
#define CODE_ENTRY_OFFSET(slots)
#define RENDER_IRAM_ATTR
#define RENDER_SKIP_ATTR
#define RENDER_DIVIDE_ATTR
#define RENDER_TAIL_ATTR
#endif
