/* console_heap: HEAP prints the free bytes, largest free block, total size and
 * low-water mark of MEMORY_DMA, the heap POST and HEAPMARK report, or with
 * "psram" of MEMORY_PSRAM. "HEAP reset" restarts every low-water mark, so the
 * next reading's min= covers only what ran since. Development builds only. */

#include "console/console_verbs.h"

#include "core/memory.h"

#include <stdio.h>
#include <string.h>

static void
print_heap(memory_kind_t kind) {
    (void)printf("HEAP free=%zu largest=%zu total=%zu min=%zu\n", memory_free_bytes(kind), memory_largest_block(kind),
                 memory_total_bytes(kind), memory_min_free_bytes(kind));
}

static void
console_verb_heap(const char* args, console_reply_fn reply) {
    if (args[0] == '\0') {
        print_heap(MEMORY_DMA);
    } else if (strcmp(args, "psram") == 0) {
        print_heap(MEMORY_PSRAM);
    } else if (strcmp(args, "reset") == 0) {
        memory_min_free_restart();
        (void)printf("HEAP reset\n");
    } else {
        reply("HEAP_ERR want nothing, psram or reset");
    }
    (void)fflush(stdout);
}

CONSOLE_VERB(heap, sizeof "psram" - 1, console_verb_heap)
