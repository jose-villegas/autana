/* Tone detection and phase-difference frequency measurement. */
#pragma once
#include <stdbool.h>
#include <stdint.h>

#include "audio/audio_out.h"

/* Trim the first captured samples after audio starts. */
#define AUDIO_LOOPBACK_SETTLE_MS       100u
#define AUDIO_LOOPBACK_HEARD_MARGIN_DB 20.0F

static inline size_t
audio_loopback_sample_budget(uint32_t ms) {
    return audio_out_samples(AUDIO_LOOPBACK_SETTLE_MS) + audio_out_samples(ms);
}

/* Blocking; call from a worker or console task, never the frame loop. */
bool audio_loopback_measure(uint16_t hz, uint16_t ms, float* db, float* measured_hz);
