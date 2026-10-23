/* Records are owned by their calling translation units and registered on demand. */
#include "math/scalar/math_calls.h"

#if defined(CONFIG_MATH_COUNT_CALLS) && CONFIG_MATH_COUNT_CALLS
#include <stdio.h>

static _Atomic(math_call_record_t*) records;
static atomic_bool measuring;

void
math_calls_hit(math_call_record_t* record) {
    if (!atomic_load_explicit(&measuring, memory_order_acquire)) {
        return;
    }
    if (!atomic_exchange_explicit(&record->registered, true, memory_order_relaxed)) {
        math_call_record_t* head = atomic_load_explicit(&records, memory_order_acquire);
        do {
            record->next = head;
        } while (!atomic_compare_exchange_weak_explicit(&records, &head, record, memory_order_acq_rel,
                                                        memory_order_acquire));
    }
    atomic_fetch_add_explicit(&record->count, 1, memory_order_relaxed);
}

void
math_calls_begin(void) {
    for (math_call_record_t* record = atomic_load_explicit(&records, memory_order_acquire); record != NULL;
         record = record->next) {
        atomic_store_explicit(&record->count, 0, memory_order_relaxed);
    }
    atomic_store_explicit(&measuring, true, memory_order_release);
}

void
math_calls_report(uint32_t frames) {
    atomic_store_explicit(&measuring, false, memory_order_release);
    if (frames == 0) {
        return;
    }
    printf("MATHCALLS begin\n");
    for (math_call_record_t* record = atomic_load_explicit(&records, memory_order_acquire); record != NULL;
         record = record->next) {
        const unsigned count = atomic_load_explicit(&record->count, memory_order_relaxed);
        printf("MATHCALLS fn=%s caller=%s per_frame=%g\n", record->function, record->caller,
               (double)((float)count / (float)frames));
    }
    printf("MATHCALLS end\n");
}
#endif
