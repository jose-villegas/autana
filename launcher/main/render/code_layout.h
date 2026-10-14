#pragma once

/* Where an inner loop sits within an instruction-cache line moves the frame
 * time. Render functions in flash start on a line; this puts `slots` two-byte
 * nop.n, never executed, ahead of a hot one's entry so its loops land at the
 * offsets that ran fastest. code_layout.txt pins them; measure and retune
 * changed layouts on the board before regenerating it. */
#define RENDER_ENTRY_OFFSET(slots) __attribute__((patchable_function_entry(slots, slots)))

#ifdef ESP_PLATFORM
#include "esp_attr.h"
/* The draw path's entry functions run from IRAM: on a dense scene, flash
 * fetch stalls cost the drawing core about a tenth of its draw cycles. IRAM
 * has no cache line to place a loop on, so the render sources' cache-line
 * function alignment would only waste IRAM; 4 bytes keeps entries aligned.
 * IRAM budget: 5401 B */
#define RENDER_IRAM_ATTR IRAM_ATTR __attribute__((aligned(4)))
#else
#define RENDER_IRAM_ATTR
#endif
