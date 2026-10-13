/*
 * console_heap: development-only free bytes and largest allocation in
 * MEMORY_DMA, the internal pool measured by POST and HEAPMARK.
 */
#include "console/console_verbs.h"

#include "core/memory.h"

#include <stdio.h>

static void
console_verb_heap(const char* args, console_reply_fn reply) {
    (void)args;
    (void)reply;
    printf("HEAP free=%zu largest=%zu\n", memory_free_bytes(MEMORY_DMA), memory_largest_block(MEMORY_DMA));
    fflush(stdout);
}

CONSOLE_VERB(heap, 0, console_verb_heap)
