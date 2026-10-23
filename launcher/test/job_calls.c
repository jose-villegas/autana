#include "job_calls.h"

#include "core/job.h"

bool __real_job_try_core1(job_fn_t fn, const void* ctx, size_t ctx_size);
bool __wrap_job_try_core1(job_fn_t fn, const void* ctx, size_t ctx_size);
bool __real_job_run_steered(int16_t* core1_offset, job_fn_t fn, void* first_half, const void* second_half,
                            size_t ctx_size);
bool __wrap_job_run_steered(int16_t* core1_offset, job_fn_t fn, void* first_half, const void* second_half,
                            size_t ctx_size);

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

/* Its own dispatch stays inside job.c, out of the wrap's reach. */
bool
__wrap_job_run_steered(int16_t* core1_offset, job_fn_t fn, void* first_half, const void* second_half, size_t ctx_size) {
    calls++;
    return __real_job_run_steered(core1_offset, fn, first_half, second_half, ctx_size);
}
