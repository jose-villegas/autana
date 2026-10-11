/* A core-1 job is one copied callback context and no queue. */
#pragma once

#include <assert.h>
#include <stdbool.h>
#include <stddef.h>

#define JOB_CTX_MAX 128

typedef void (*job_fn_t)(void* ctx);

/* Called with the core-1 worker's task once it exists: now, when it already
 * does. One at a time; a later call replaces it. Never called on a host. */
void job_on_worker_started(void (*fn)(void* task));

/* Runs a valid job on core 1 when its worker is idle; otherwise runs it
 * inline. The context is copied before either path calls fn. False means
 * ctx_size exceeded JOB_CTX_MAX and fn was not called. */
bool job_run_core1(job_fn_t fn, const void* ctx, size_t ctx_size);

/* Dispatches to core 1 or not at all: false means the caller must do the work
 * itself. For a job that blocks on progress the caller makes, where
 * job_run_core1()'s inline fallback would wait forever. Never true on a
 * host. */
bool job_try_core1(job_fn_t fn, const void* ctx, size_t ctx_size);

/* Waits for the dispatched core-1 job. False retains it for a later wait;
 * later jobs run inline in the meantime. */
bool job_wait(unsigned timeout_ms);

#define JOB_SPLIT_WAIT_MS 1000U

/* The second context is copied on dispatch. Both ranges finish before the next stage reads. */
static inline __attribute__((always_inline)) void
job_run_split(job_fn_t fn, void* first_half, const void* second_half, size_t ctx_size) {
    if (!job_try_core1(fn, second_half, ctx_size)) {
        fn((void*)second_half);
    }
    fn(first_half);
    const bool done = job_wait(JOB_SPLIT_WAIT_MS);
    assert(done); /* the next stage reads what core 1 wrote */
    (void)done;
}
