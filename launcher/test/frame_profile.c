#include "frame_profile.h"
#include <limits.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include "collections/strbuf.h"
#include "core/memory.h"
#include "esp_log.h"
#ifdef DEVICE_BUILD
#include "console/console.h"
#endif

static frame_profile_t
play(const frame_profile_scenario_t* s, frame_profile_frame_fn frame, void* ctx, int32_t* samples, int capacity,
     frame_profile_frame_t* traced) {
    frame_profile_t p = {0};
    int64_t previous_us = frame(ctx, 0);
    if (previous_us <= 0 || previous_us > INT32_MAX) {
        ESP_LOGE("frame_profile", "%s: invalid warm-up frame time", s->row);
        return p;
    }
    int64_t elapsed_us = 0;
    while (elapsed_us < s->duration_us) {
        if (p.frames == capacity) {
            ESP_LOGE("frame_profile", "%s: sample capacity exhausted", s->row);
            return (frame_profile_t){0};
        }
        elapsed_us += previous_us;
        const int64_t us = frame(ctx, previous_us);
        if (us <= 0 || us > INT32_MAX) {
            ESP_LOGE("frame_profile", "%s: invalid frame time", s->row);
            return (frame_profile_t){0};
        }
        if (traced != NULL) {
            traced[p.frames] = (frame_profile_frame_t){(int32_t)(elapsed_us / MATH_US_PER_MS), (int32_t)us};
        }
        samples[p.frames++] = (int32_t)us;
        p.total_us += us;
        previous_us = us;
    }
    p.frame_us = bench_stats_compute(samples, p.frames);
    return p;
}

frame_profile_t
frame_profile_play(const frame_profile_scenario_t* s, frame_profile_frame_fn frame, void* ctx,
                   frame_profile_trace_t* trace) {
    if (trace != NULL) {
        *trace = (frame_profile_trace_t){0};
    }
    if (s->duration_us <= 0 || s->duration_us / FRAME_PROFILE_MIN_FRAME_US >= INT_MAX - 1) {
        ESP_LOGE("frame_profile", "invalid scenario duration");
        return (frame_profile_t){0};
    }
    const int capacity = (int)((s->duration_us - 1) / FRAME_PROFILE_MIN_FRAME_US) + 2;
    int32_t* samples = memory_alloc((size_t)capacity * sizeof *samples, MEMORY_PSRAM);
    frame_profile_frame_t* traced =
        trace == NULL ? NULL : memory_alloc((size_t)capacity * sizeof *traced, MEMORY_PSRAM);
    if (samples == NULL || (trace != NULL && traced == NULL)) {
        ESP_LOGE("frame_profile", "%s: sample allocation failed", s->row);
        memory_free(samples);
        memory_free(traced);
        return (frame_profile_t){0};
    }
    const frame_profile_t p = play(s, frame, ctx, samples, capacity, traced);
    memory_free(samples);
    if (trace != NULL && p.frames > 0) {
        *trace = (frame_profile_trace_t){traced, p.frames};
    } else {
        memory_free(traced);
    }
    return p;
}

void
frame_profile_trace_free(frame_profile_trace_t* trace) {
    memory_free(trace->frame);
    *trace = (frame_profile_trace_t){0};
}

/* The lowest frame walking from `at` by `step` before one higher than `at` (or equal, `equal_stops`);
 * false with none on the whole loop. */
static bool
saddle(const frame_profile_trace_t* trace, int at, int step, bool equal_stops, int32_t* lowest) {
    const int32_t height = trace->frame[at].frame_us;
    int32_t low = height;
    for (int walked = 1, i = at; walked < trace->frames; walked++) {
        i = (i + step + trace->frames) % trace->frames;
        const int32_t us = trace->frame[i].frame_us;
        if (us > height || (equal_stops && us == height)) {
            *lowest = low;
            return true;
        }
        low = us < low ? us : low;
    }
    return false;
}

/* Inserts `p` by height among the `count` filled of `capacity`, after equal ones. */
static void
rank_peak(const frame_profile_trace_t* trace, frame_profile_peak_t p, frame_profile_peak_t* peak, int count,
          int capacity) {
    int at = count < capacity ? count : capacity;
    while (at > 0 && trace->frame[p.frame].frame_us > trace->frame[peak[at - 1].frame].frame_us) {
        if (at < capacity) {
            peak[at] = peak[at - 1];
        }
        at--;
    }
    if (at < capacity) {
        peak[at] = p;
    }
}

/* The slowest frame of `trace`, the earliest of equal ones, and its lowest frame time. */
static void
loop_extremes(const frame_profile_trace_t* trace, int* slowest, int32_t* lowest) {
    *slowest = 0;
    *lowest = trace->frame[0].frame_us;
    for (int i = 1; i < trace->frames; i++) {
        *slowest = trace->frame[i].frame_us > trace->frame[*slowest].frame_us ? i : *slowest;
        *lowest = trace->frame[i].frame_us < *lowest ? trace->frame[i].frame_us : *lowest;
    }
}

/* A peak is a run of frames above the frame before it and not below the one after, standing for its
 * first frame. */
static bool
is_peak(const frame_profile_trace_t* trace, int at) {
    const int32_t height = trace->frame[at].frame_us;
    const int32_t before = trace->frame[(at - 1 + trace->frames) % trace->frames].frame_us;
    const int32_t after = trace->frame[(at + 1) % trace->frames].frame_us;
    return before < height && after <= height;
}

/* The peak's height over the higher of its two saddles; an equal frame stops only the left walk, so a
 * plateau counts once. With no higher frame on either side, the saddle is the loop's `lowest`. */
static int32_t
prominence(const frame_profile_trace_t* trace, int at, int32_t lowest) {
    int32_t left = lowest;
    int32_t right = lowest;
    const bool has_left = saddle(trace, at, -1, true, &left);
    const bool has_right = saddle(trace, at, 1, false, &right);
    if (has_left != has_right) {
        return trace->frame[at].frame_us - (has_left ? left : right);
    }
    return trace->frame[at].frame_us - (left > right ? left : right);
}

int
frame_profile_peaks(const frame_profile_trace_t* trace, frame_profile_peak_t* peak, int capacity) {
    if (trace->frames == 0) {
        return 0;
    }
    int slowest = 0;
    int32_t lowest = 0;
    loop_extremes(trace, &slowest, &lowest);
    int count = 0;
    for (int i = 0; i < trace->frames; i++) {
        const int32_t rise = is_peak(trace, i) ? prominence(trace, i, lowest) : 0;
        if (rise > 0
            && (int64_t)rise * FRAME_PROFILE_PERCENT
                   >= (int64_t)FRAME_PROFILE_PEAK_PERCENT * trace->frame[i].frame_us) {
            if (peak != NULL) {
                rank_peak(trace, (frame_profile_peak_t){i, rise}, peak, count, capacity);
            }
            count++;
        }
    }
    if (count == 0 && peak != NULL && capacity > 0) {
        peak[0] = (frame_profile_peak_t){slowest, trace->frame[slowest].frame_us - lowest};
    }
    return count == 0 ? 1 : count;
}

/* `text` starts with its tag's `prefix` bytes. On the board it goes through the console's waiting writer,
 * after what stdio holds, so nothing is lost or reordered. */
static void
emit(const char* tag, const char* text, size_t prefix) {
#ifdef DEVICE_BUILD
    (void)tag;
    (void)prefix;
    (void)fflush(stdout);
    console_emit_line("", text);
#else
    ESP_LOGI(tag, "%s", text + prefix);
#endif
}

void
frame_profile_log_trace(const char* tag, const char* row, const frame_profile_trace_t* trace,
                        const frame_profile_peak_t* peak, int count, const char* quarter) {
    const size_t frames_size = FRAME_PROFILE_LINE_FRAMES * sizeof(" -2147483648:-2147483648");
    /* The trace line is the longer: the format with its row, count and quarter, after the tag. */
    const size_t size = strlen(tag) + sizeof(": " FRAME_PROFILE_TRACE " " FRAME_PROFILE_QUARTER) + strlen(row)
                        + sizeof("-2147483648") + frames_size + strlen(quarter);
    char* frames = memory_alloc(frames_size, MEMORY_INTERNAL);
    char* text = memory_alloc(size, MEMORY_INTERNAL);
    if (frames == NULL || text == NULL) {
        ESP_LOGE(tag, "%s: trace line allocation failed", row);
        memory_free(frames);
        memory_free(text);
        return;
    }
    const size_t prefix = (size_t)snprintf(text, size, "%s: ", tag);
    for (int first = 0; first < trace->frames; first += FRAME_PROFILE_LINE_FRAMES) {
        const int n =
            trace->frames - first < FRAME_PROFILE_LINE_FRAMES ? trace->frames - first : FRAME_PROFILE_LINE_FRAMES;
        strbuf_t line = strbuf_make(frames, frames_size);
        for (int i = first; i < first + n; i++) {
            strbuf_appendf(&line, FRAME_PROFILE_TRACE_FRAME, (int)trace->frame[i].path_ms,
                           (int)trace->frame[i].frame_us);
        }
        (void)snprintf(text + prefix, size - prefix, FRAME_PROFILE_TRACE " " FRAME_PROFILE_QUARTER "%s", row, n, frames,
                       quarter);
        emit(tag, text, prefix);
    }
    for (int rank = 0; rank < count; rank++) {
        const frame_profile_frame_t* f = &trace->frame[peak[rank].frame];
        (void)snprintf(text + prefix, size - prefix, FRAME_PROFILE_PEAK " " FRAME_PROFILE_QUARTER "%s", row, rank + 1,
                       (int)f->path_ms, (int)f->frame_us, (int)peak[rank].prominence_us, quarter);
        emit(tag, text, prefix);
    }
    memory_free(text);
    memory_free(frames);
}

void
frame_profile_log(const char* tag, const char* row, const frame_profile_t* p, const char* quarter) {
    if (p->frames == 0) {
        ESP_LOGE(tag, FRAME_PROFILE_PREFIX " %s failed", row);
        return;
    }
    ESP_LOGI(tag,
             FRAME_PROFILE_PREFIX " %s " FRAME_PROFILE_FRAMES "%d " FRAME_PROFILE_MEAN " %lldus " FRAME_PROFILE_MED
                                  " %lldus " FRAME_PROFILE_P95 " %lldus " FRAME_PROFILE_WORST
                                  " %lldus " FRAME_PROFILE_BEST " %lldus | " FRAME_PROFILE_FPS " " FRAME_PROFILE_MEAN
                                  " %.2f " FRAME_PROFILE_LOW " %.2f " FRAME_PROFILE_WORST " %.2f " FRAME_PROFILE_QUARTER
                                  "%s",
             row, p->frames, (long long)p->frame_us.avg, (long long)p->frame_us.med, (long long)p->frame_us.p95,
             (long long)p->frame_us.max, (long long)p->frame_us.min, (double)MATH_US_PER_S * p->frames / p->total_us,
             (double)MATH_US_PER_S / p->frame_us.p95, (double)MATH_US_PER_S / p->frame_us.max, quarter);
}
