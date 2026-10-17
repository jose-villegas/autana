/* Sample-rate owner for blocking mono PCM output. The amplifier is off when
 * idle; callers serialize audio operations and keep them off the frame loop. */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "math/scalar/units.h"

#define AUDIO_OUT_SAMPLE_RATE_HZ 22050
#define AUDIO_OUT_NYQUIST_HZ     (AUDIO_OUT_SAMPLE_RATE_HZ / 2)
#define AUDIO_OUT_MAX_MS         60000u
#define AUDIO_OUT_VOLUME_MAX_PCT 100u

static inline size_t
audio_out_samples(uint32_t ms) {
    return (size_t)((uint64_t)ms * AUDIO_OUT_SAMPLE_RATE_HZ / MATH_MS_PER_S);
}

static inline bool
audio_out_args_valid(uint32_t hz, uint32_t ms, uint32_t volume_pct) {
    return hz > 0 && hz < AUDIO_OUT_NYQUIST_HZ && ms > 0 && ms <= AUDIO_OUT_MAX_MS
           && volume_pct <= AUDIO_OUT_VOLUME_MAX_PCT;
}

bool audio_out_open(void);
/* Blocking; never call from the frame task (frame_watch_task()). */
bool audio_out_tone(uint16_t hz, uint16_t ms, uint8_t volume_pct);
/* The last input/output close releases the shared codec and I2S channels. */
void audio_out_close(void);
