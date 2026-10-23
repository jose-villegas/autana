/* sort: stable in-place ordering of a small array by a qsort-shaped comparator, inline so the comparator inlines. */
#pragma once

#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* Negative when a goes before b, positive when after, zero when either order will do: qsort's contract. */
typedef int (*sort_compare_t)(const void* a, const void* b);

/* The largest element sort_insertion moves, held on the stack while it moves. */
#define SORT_ELEMENT_MAX 32

/* Insertion sort: O(n^2) compares, so for the tens of elements a caller holds, where it beats qsort and keeps
 * equal elements in their order, which qsort does not promise. */
static inline void
sort_insertion(void* base, size_t count, size_t size, sort_compare_t compare) {
    assert(size <= SORT_ELEMENT_MAX);
    uint8_t* const bytes = base;
    uint8_t moving[SORT_ELEMENT_MAX];
    for (size_t i = 1; i < count; i++) {
        memcpy(moving, bytes + (i * size), size);
        size_t at = i;
        while (at > 0 && compare(bytes + ((at - 1) * size), moving) > 0) {
            memmove(bytes + (at * size), bytes + ((at - 1) * size), size);
            at--;
        }
        memcpy(bytes + (at * size), moving, size);
    }
}

static inline int
sort_compare_int(const void* a, const void* b) {
    const int x = *(const int*)a;
    const int y = *(const int*)b;
    return (x > y) - (x < y);
}
