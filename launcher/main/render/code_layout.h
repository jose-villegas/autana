#pragma once

/* Where an inner loop sits within an instruction-cache line moves the frame
 * time. Render functions start on a line; this puts `slots` two-byte nop.n,
 * never executed, ahead of a hot one's entry so its loops land at the offsets
 * that ran fastest. Editing such a function moves its loops: re-check them
 * with launcher/tools/render/code_layout.py and on the board. */
#define RENDER_ENTRY_OFFSET(slots) __attribute__((patchable_function_entry(slots, slots)))
