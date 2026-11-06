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
/* On a dense scene, flash fetch stalls cost the drawing core about a tenth
 * of its draw cycles. Per-triangle set-up joins drawing in IRAM so unrelated
 * code moves cannot shift its flash cache sets. IRAM has no cache line to
 * place a loop on, so the render sources' cache-line alignment would only
 * waste IRAM; 4 bytes keeps entries aligned.
 * IRAM budget: 8588 B */
#define RENDER_IRAM_ATTR IRAM_ATTR __attribute__((aligned(4)))
/* The draw path's capped copies for a view that skips clusters behind drawn
 * depth (render_view_t.skip_hidden). IRAM is granted per measured ms saved
 * per KB, with the largest free internal block noted alongside.
 * IRAM budget: 5135 B */
#define RENDER_SKIP_ATTR RENDER_IRAM_ATTR
#else
/* A host build has no instruction cache to place code in. */
#define CODE_ENTRY_OFFSET(slots)
#define RENDER_IRAM_ATTR
#define RENDER_SKIP_ATTR
#endif
