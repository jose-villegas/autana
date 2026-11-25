/* sort: stable in-place insertion sort of small arrays. SORT_DEFINE_INSERTION makes one per element type, moving
 * elements by value so a small one stays in registers; sort_insertion serves records whose size is known only at run
 * time. O(n^2) compares, so for the tens of elements a caller holds, where it beats qsort and keeps equal elements in
 * their order, which qsort does not promise. */
#pragma once

#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* NAME_insert(items, count, item) puts item into the sorted items[0..count), after any equal to it, so items needs
 * room for count + 1; NAME_sort(items, count) sorts in place. BEFORE(a, b) is true when a must go before b. */
#define SORT_DEFINE_INSERTION(NAME, T, BEFORE)                                                                         \
    static inline void NAME##_insert(T* items, size_t count, T item) {                                                 \
        size_t at = count;                                                                                             \
        while (at > 0 && BEFORE(item, items[at - 1])) {                                                                \
            items[at] = items[at - 1];                                                                                 \
            at--;                                                                                                      \
        }                                                                                                              \
        items[at] = item;                                                                                              \
    }                                                                                                                  \
    static inline void NAME##_sort(T* items, size_t count) {                                                           \
        for (size_t i = 1; i < count; i++) {                                                                           \
            NAME##_insert(items, i, items[i]);                                                                         \
        }                                                                                                              \
    }

/* Returns -1 when every key exceeds the search value or the array is empty. */
#define SORT_DEFINE_LAST_AT_MOST(NAME, T)                                                                              \
    static inline int NAME(const T* keys, int count, T value) {                                                        \
        int lo = 0;                                                                                                    \
        int hi = count;                                                                                                \
        while (lo < hi) {                                                                                              \
            const int mid = lo + (hi - lo) / 2;                                                                        \
            if (keys[mid] <= value) {                                                                                  \
                lo = mid + 1;                                                                                          \
            } else {                                                                                                   \
                hi = mid;                                                                                              \
            }                                                                                                          \
        }                                                                                                              \
        return lo - 1;                                                                                                 \
    }

/* Negative when a goes before b, positive when after, zero when either order will do: qsort's contract. */
typedef int (*sort_compare_t)(const void* a, const void* b);

/* The largest element sort_insertion moves, held on the stack while it moves. */
#define SORT_ELEMENT_MAX 32

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
