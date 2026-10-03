#pragma once

/* Where an inner loop sits within an instruction-cache line moves the frame
 * time. Render functions start on a line; this puts `slots` two-byte nop.n,
 * never executed, ahead of a hot one's entry so its loops land at the offsets
 * that ran fastest. launcher/tools/build/build_diag_check.sh checks
 * code_layout.txt; measure and
 * retune changed layouts on the board before regenerating it. */
#define RENDER_ENTRY_OFFSET(slots) __attribute__((patchable_function_entry(slots, slots)))
