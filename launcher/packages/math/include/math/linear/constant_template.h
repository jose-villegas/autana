/* constant_template: named aggregate constants shared by math families. */
#pragma once

#define MATH_DEFINE_CONSTANT(P, NAME, ...)                                                                             \
    static inline P##_t P##_##NAME(void) { return (P##_t){__VA_ARGS__}; }

#define MATH_DEFINE_CONSTANT_ALIAS(P, NAME, TARGET)                                                                    \
    static inline P##_t P##_##NAME(void) { return P##_##TARGET(); }
