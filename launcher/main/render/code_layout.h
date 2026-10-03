#pragma once

/* Xtensa emits a two-byte nop.n before the entry for each requested slot. */
#define RENDER_ENTRY_OFFSET(slots) __attribute__((patchable_function_entry(slots, slots)))
