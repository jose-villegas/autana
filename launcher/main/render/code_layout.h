#pragma once

/* Where an inner loop sits within an instruction-cache line moves the frame
 * time. Render functions start on a line; this puts `slots` two-byte nop.n,
 * never executed, ahead of a hot one's entry so its loops land at the offsets
 * that ran fastest. launcher/tools/build/build_diag_check.sh checks
 * code_layout.txt; measure and
 * retune changed layouts on the board before regenerating it. */
#define RENDER_ENTRY_OFFSET(slots) __attribute__((patchable_function_entry(slots, slots)))

#ifdef ESP_PLATFORM
#include "esp_attr.h"
/* The draw path's entry functions run from IRAM: on a Sponza flythrough
 * flash fetch stalls cost core 0 about a tenth of its r3d.draw cycles. IRAM
 * has no cache line to place a loop on, and 4-byte alignment (the fetch
 * width) replaces the render sources' cache-line function alignment.
 * IRAM budget: release 7241 B, dev 7385 B, diag 7385 B */
#define RENDER_IRAM_ATTR IRAM_ATTR __attribute__((aligned(4)))
#else
#define RENDER_IRAM_ATTR
#endif
