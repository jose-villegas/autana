#pragma once

/* Where an inner loop sits within an instruction-cache line moves the frame
 * time. A pinned function starts on a line, whatever code links ahead of it;
 * `slots` two-byte nop.n, never executed, then sit ahead of its entry so its
 * loops land at the offsets that ran fastest. code_layout.py checks the
 * owner's code_layout.txt; measure and retune changed layouts on the board
 * before regenerating it. */

#include "sdkconfig.h"

#define CODE_ENTRY_OFFSET(slots)                                                                                       \
    __attribute__((aligned(CONFIG_ESP32S3_INSTRUCTION_CACHE_LINE_SIZE), patchable_function_entry(slots, slots)))
