#include "frame_profile.h"
#include <limits.h>
#include "core/memory.h"
#include "esp_log.h"

frame_profile_t
frame_profile_play(const frame_profile_scenario_t* s, frame_profile_frame_fn frame, void* ctx) {
    frame_profile_t p = {0};
    if (s->duration_us <= 0 || s->duration_us / FRAME_PROFILE_MIN_FRAME_US >= INT_MAX - 1) {
        ESP_LOGE("frame_profile", "invalid scenario duration");
        return p;
    }
    const int capacity = (int)((s->duration_us - 1) / FRAME_PROFILE_MIN_FRAME_US) + 2;
    int32_t* samples = memory_alloc((size_t)capacity * sizeof *samples, MEMORY_PSRAM);
    if (samples == NULL) {
        ESP_LOGE("frame_profile", "%s: sample allocation failed", s->row);
        return p;
    }
    int64_t previous_us = frame(ctx, 0);
    if (previous_us <= 0 || previous_us > INT32_MAX) {
        ESP_LOGE("frame_profile", "%s: invalid warm-up frame time", s->row);
        memory_free(samples);
        return p;
    }
    int64_t elapsed_us = 0;
    while (elapsed_us < s->duration_us) {
        if (p.frames == capacity) {
            ESP_LOGE("frame_profile", "%s: sample capacity exhausted", s->row);
            memory_free(samples);
            return (frame_profile_t){0};
        }
        elapsed_us += previous_us;
        const int64_t us = frame(ctx, previous_us);
        if (us <= 0 || us > INT32_MAX) {
            ESP_LOGE("frame_profile", "%s: invalid frame time", s->row);
            memory_free(samples);
            return (frame_profile_t){0};
        }
        samples[p.frames++] = (int32_t)us;
        p.total_us += us;
        previous_us = us;
    }
    p.frame_us = perf_stats_compute(samples, p.frames);
    memory_free(samples);
    return p;
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
