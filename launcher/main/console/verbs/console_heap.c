/* console_heap: HEAP prints the free bytes, largest free block and total size
 * of MEMORY_DMA, the heap POST and HEAPMARK report. Development builds only. */

#include "console/console_verbs.h"

#include "core/memory.h"

#include <stdio.h>

static void
console_verb_heap(const char* args, console_reply_fn reply) {
    (void)args;
    (void)reply;
    (void)printf("HEAP free=%zu largest=%zu total=%zu\n", memory_free_bytes(MEMORY_DMA),
                 memory_largest_block(MEMORY_DMA), memory_total_bytes(MEMORY_DMA));
    (void)fflush(stdout);
}

CONSOLE_VERB(heap, 0, console_verb_heap)
