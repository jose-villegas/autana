#include "job_calls.h"

#include "core/job.h"

bool __real_job_try_core1(job_fn_t fn, const void* ctx, size_t ctx_size);
bool __wrap_job_try_core1(job_fn_t fn, const void* ctx, size_t ctx_size);

static int calls;

int
job_calls_count(void) {
    return calls;
}

bool
__wrap_job_try_core1(job_fn_t fn, const void* ctx, size_t ctx_size) {
    calls++;
    return __real_job_try_core1(fn, ctx, ctx_size);
}
