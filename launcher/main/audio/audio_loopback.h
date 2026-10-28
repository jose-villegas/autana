/* Tone detection, phase-difference frequency, and THD over harmonics 2..5 below Nyquist. */
#pragma once
#include <stdbool.h>
#include <stdint.h>

#include "audio/audio_out.h"

#define AUDIO_LOOPBACK_THD_FLOOR_DB   (-60.0F)
#define AUDIO_LOOPBACK_HARMONICS      5u
#define AUDIO_LOOPBACK_FIRST_HARMONIC 2u

typedef struct {
    float measured_hz;
    float thd_db;
    float peak_dbfs;
    float noise_dbfs;
} audio_loopback_result_t;

/* Trim the first captured samples after audio starts. */
#define AUDIO_LOOPBACK_SETTLE_MS       100u
#define AUDIO_LOOPBACK_HEARD_MARGIN_DB 20.0F

static inline size_t
audio_loopback_sample_budget(uint32_t ms) {
    return audio_out_samples(AUDIO_LOOPBACK_SETTLE_MS) + audio_out_samples(ms);
}

/* Blocking; call from a worker or console task, never the frame loop. */
bool audio_loopback_measure(uint16_t hz, uint16_t ms, float* db, audio_loopback_result_t* result);
