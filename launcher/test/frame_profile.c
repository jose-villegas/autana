#include "frame_profile.h"
#include <limits.h>
#include "core/memory.h"
#include "esp_log.h"

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

int
frame_profile_worst(const frame_profile_trace_t* trace, int k, int* index) {
    int count = 0;
    for (int i = 0; i < trace->frames; i++) {
        int at = count < k ? count++ : k;
        while (at > 0 && trace->frame[i].frame_us > trace->frame[index[at - 1]].frame_us) {
            if (at < k) {
                index[at] = index[at - 1];
            }
            at--;
        }
        if (at < k) {
            index[at] = i;
        }
    }
    return count;
}

void
frame_profile_log_trace(const char* tag, const char* row, const frame_profile_trace_t* trace, const int* worst,
                        int count, const char* quarter) {
    for (int i = 0; i < trace->frames; i++) {
        ESP_LOGI(tag, FRAME_PROFILE_TRACE " " FRAME_PROFILE_QUARTER "%s", row, (int)trace->frame[i].path_ms,
                 (int)trace->frame[i].frame_us, quarter);
    }
    for (int rank = 0; rank < count; rank++) {
        const frame_profile_frame_t* f = &trace->frame[worst[rank]];
        ESP_LOGI(tag, FRAME_PROFILE_RANKED " " FRAME_PROFILE_QUARTER "%s", row, rank + 1, (int)f->path_ms,
                 (int)f->frame_us, quarter);
    }
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
