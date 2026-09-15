/* A core-1 job is one copied callback context and no queue. */
#pragma once

#include <stdbool.h>
#include <stddef.h>

#define JOB_CTX_MAX 128

typedef void (*job_fn_t)(void* ctx);

/* Runs a valid job on core 1 when its worker is idle; otherwise runs it
 * inline. The context is copied before either path calls fn. False means
 * ctx_size exceeded JOB_CTX_MAX and fn was not called. */
bool job_run_core1(job_fn_t fn, const void* ctx, size_t ctx_size);

/* Waits for the dispatched core-1 job. False retains it for a later wait;
 * later jobs run inline in the meantime. */
bool job_wait(unsigned timeout_ms);
