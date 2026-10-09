/* The critical section only: a spinlock that really excludes, so a host
 * test racing two threads through it sees what the board's portMUX gives. */
#pragma once
#include <stdatomic.h>

typedef struct {
    atomic_flag held;
} portMUX_TYPE;

#define portMUX_INITIALIZER_UNLOCKED {ATOMIC_FLAG_INIT}

static inline void
portENTER_CRITICAL(portMUX_TYPE* mux) {
    while (atomic_flag_test_and_set_explicit(&mux->held, memory_order_acquire)) {}
}

static inline void
portEXIT_CRITICAL(portMUX_TYPE* mux) {
    atomic_flag_clear_explicit(&mux->held, memory_order_release);
}
