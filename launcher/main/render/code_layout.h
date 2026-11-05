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
/* Per-triangle setup and drawing run from IRAM to avoid flash fetch stalls
 * and cache-set placement effects; 4-byte alignment avoids wasting IRAM on
 * the render sources' cache-line function alignment.
 * IRAM budget: 8588 B */
#define RENDER_IRAM_ATTR IRAM_ATTR __attribute__((aligned(4)))
/* A view that skips clusters behind drawn depth also runs its capped setup
 * and drawing in IRAM, keeping the skip path free of flash fetch stalls.
 * IRAM budget: 5135 B */
#define RENDER_SKIP_ATTR RENDER_IRAM_ATTR
#else
/* A host build has no instruction cache to place code in. */
#define CODE_ENTRY_OFFSET(slots)
#define RENDER_IRAM_ATTR
#define RENDER_SKIP_ATTR
#endif
