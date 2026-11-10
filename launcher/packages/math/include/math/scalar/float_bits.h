/* float_bits: bit-preserving transfers between float and uint32_t. */
#pragma once

#include <float.h>
#include <stdint.h>
#include <string.h>

/* Xtensa bit transfers avoid spilling float values through the stack. */
static inline __attribute__((always_inline)) uint32_t
mathf_bits(float value) {
#ifdef __XTENSA__
    uint32_t bits;
    __asm__("rfr %0, %1" : "=a"(bits) : "f"(value));
    return bits;
#else
    uint32_t bits;
    memcpy(&bits, &value, sizeof bits);
    return bits;
#endif
}

static inline __attribute__((always_inline)) float
mathf_from_bits(uint32_t bits) {
#ifdef __XTENSA__
    float value;
    __asm__("wfr %0, %1" : "=f"(value) : "a"(bits));
    return value;
#else
    float value;
    memcpy(&value, &bits, sizeof value);
    return value;
#endif
}

/* For positive normal a, the e with a in [2^(e-1), 2^e): frexpf's exponent,
 * read off the bits without a libm call. */
static inline __attribute__((always_inline)) int
mathf_exponent(float a) {
    const uint32_t field = ((uint32_t)FLT_MAX_EXP * 2U) - 1U; /* the exponent's bits, all ones */
    return (int)((mathf_bits(a) >> (FLT_MANT_DIG - 1)) & field) - (FLT_MAX_EXP - 2);
}
