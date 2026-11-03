/* A core-1 job is one copied callback context and no queue. */
#pragma once

#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define JOB_CTX_MAX        128

/* Gfx presents at priority 5; rendering must preempt engine work. */
#define JOB_CORE1_PRIORITY 3
#define JOB_CORE1          1

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

/* Microseconds from the last dispatch until its worker finished it, so time
 * spent queued behind a higher-priority task on core 1 counts. Valid after a
 * job_wait() that returned true, until the next dispatch. 0 on a host. */
int64_t job_core1_elapsed_us(void);

#define JOB_SPLIT_WAIT_MS 1000U

/* The second context is copied on dispatch. Both ranges finish before the next stage reads.
 * Returns whether the second range ran on the worker. */
static inline __attribute__((always_inline)) bool
job_run_split(job_fn_t fn, void* first_half, const void* second_half, size_t ctx_size) {
    const bool dispatched = job_try_core1(fn, second_half, ctx_size);
    if (!dispatched) {
        fn((void*)second_half);
    }
    fn(first_half);
    const bool done = job_wait(JOB_SPLIT_WAIT_MS);
    assert(done); /* the next stage reads what core 1 wrote */
    (void)done;
    return dispatched;
}

/* A split's share of work for core 1, in permille. Caller-owned shares are
 * offsets from even, so zeroed state splits evenly. */
#define JOB_SHARE_PERMILLE 1000
#define JOB_SHARE_EVEN     (JOB_SHARE_PERMILLE / 2)
/* Keeps either core from being starved by one noisy frame. */
#define JOB_SHARE_MIN      150
#define JOB_SHARE_MAX      (JOB_SHARE_PERMILLE - JOB_SHARE_MIN)

/* Half-damped core-1 share that balances these halves' times.
 * Nonpositive times leave the share unchanged. */
int job_share_update(int core1_permille, int64_t core1_us, int64_t caller_us);

/* Core 1's share in permille; a NULL offset is even. */
static inline int
job_share_permille(const int16_t* core1_offset) {
    return JOB_SHARE_EVEN + (core1_offset == NULL ? 0 : *core1_offset);
}

/* How many of `total` items core 1 takes at this share offset, rounded down. */
static inline int
job_share_core1_count(const int16_t* core1_offset, int total) {
    return total * job_share_permille(core1_offset) / JOB_SHARE_PERMILLE;
}

/* job_run_split() that steers *core1_offset toward equal times for both
 * halves; NULL splits evenly and steers nothing. The panel present shares
 * core 1 with the worker; a fixed split waits on it, a steered one gives
 * that core less. An empty half still runs, so its time can win work back. */
bool job_run_steered(int16_t* core1_offset, job_fn_t fn, void* first_half, const void* second_half, size_t ctx_size);

/* Work over items [from, to). */
typedef void (*job_range_fn_t)(void* ctx, int from, int to);

typedef struct {
    job_range_fn_t fn;
    void* ctx;
    int from, to;
} job_range_t;

static inline void
job_range_run(void* ctx) {
    const job_range_t* r = (const job_range_t*)ctx;
    r->fn(r->ctx, r->from, r->to);
}

/* `count` items cut at *core1_offset's share and run by job_run_steered():
 * core 1 the last ones, this core the first. Fewer than two run here. */
static inline bool
job_run_range(int16_t* core1_offset, job_range_fn_t fn, void* ctx, int count) {
    if (count < 2) {
        fn(ctx, 0, count);
        return false;
    }
    const int cut = count - job_share_core1_count(core1_offset, count);
    job_range_t first = {fn, ctx, 0, cut};
    const job_range_t second = {fn, ctx, cut, count};
    _Static_assert(sizeof second <= JOB_CTX_MAX, "a range fits a job context");
    return job_run_steered(core1_offset, job_range_run, &first, &second, sizeof second);
}
