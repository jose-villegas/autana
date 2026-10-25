/*
 * stats_template: order statistics of a sample array for one number type; see
 * docs/math/Conventions.md#templates for macro arguments. Selection is Hoare's
 * FIND (Hoare 1961, Algorithm 65): it reorders the array in place, O(n) on
 * average, and returns exactly the value a full sort would put at that rank.
 */
#pragma once
#include <stddef.h>

#define MATH_DEFINE_STATS(P, T)                                                                                        \
    /* percentile = sorted[min(count * percent / 100, count - 1)]; reorders values; count > 0. */                      \
    static inline T P##_percentile(T* values, size_t count, unsigned percent) {                                        \
        const size_t want = count * percent / 100U;                                                                    \
        const ptrdiff_t rank = (ptrdiff_t)(want < count ? want : count - 1);                                           \
        ptrdiff_t lo = 0;                                                                                              \
        ptrdiff_t hi = (ptrdiff_t)count - 1;                                                                           \
        while (lo < hi) {                                                                                              \
            const T pivot = values[rank];                                                                              \
            ptrdiff_t i = lo;                                                                                          \
            ptrdiff_t j = hi;                                                                                          \
            do {                                                                                                       \
                while (values[i] < pivot) {                                                                            \
                    i++;                                                                                               \
                }                                                                                                      \
                while (pivot < values[j]) {                                                                            \
                    j--;                                                                                               \
                }                                                                                                      \
                if (i <= j) {                                                                                          \
                    const T held = values[i];                                                                          \
                    values[i++] = values[j];                                                                           \
                    values[j--] = held;                                                                                \
                }                                                                                                      \
            } while (i <= j);                                                                                          \
            if (j < rank) {                                                                                            \
                lo = i;                                                                                                \
            }                                                                                                          \
            if (rank < i) {                                                                                            \
                hi = j;                                                                                                \
            }                                                                                                          \
        }                                                                                                              \
        return values[rank];                                                                                           \
    }                                                                                                                  \
    /* median = percentile(values, count, 50): sorted[count / 2]; reorders values; count > 0. */                       \
    static inline T P##_median(T* values, size_t count) { return P##_percentile(values, count, 50U); }
