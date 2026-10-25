/* bits: one flag per index in caller-owned bytes or 32-bit words, bit 0 first; inline so an index folds into its caller. */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define BITS8_BYTES(count)  (((size_t)(count) + 7U) / 8U)
#define BITS32_WORDS(count) (((size_t)(count) + 31U) / 32U)

static inline bool
bits8_get(const uint8_t* bits, size_t index) {
    return ((bits[index >> 3] >> (index & 7U)) & 1U) != 0U;
}

static inline void
bits8_set(uint8_t* bits, size_t index) {
    bits[index >> 3] |= (uint8_t)(1U << (index & 7U));
}

/* The same two forms as macros, for a caller whose words are a named array: a
 * store through the functions' pointer can keep the base live across a hot
 * loop and cost the caller registers. Each argument is read more than once. */
#define BITS32_GET(words, index) ((((words)[(size_t)(index) >> 5] >> ((size_t)(index) & 31U)) & 1U) != 0U)
#define BITS32_SET(words, index) ((words)[(size_t)(index) >> 5] |= UINT32_C(1) << ((size_t)(index) & 31U))

static inline bool
bits32_get(const uint32_t* bits, size_t index) {
    return BITS32_GET(bits, index);
}

static inline void
bits32_set(uint32_t* bits, size_t index) {
    BITS32_SET(bits, index);
}
