/* Sample-rate and percent-gain limits for mono PCM output. */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "math/scalar/units.h"

#ifdef ESP_PLATFORM
#include "esp_attr.h"

/* Core 0's renderer competes for the flash cache; the mixer and decoder hot
 * path avoid those fetch stalls in IRAM. ADPCM tables occupy 1440 B of DRAM.
 * IRAM budget: 1912 B */
#define AUDIO_IRAM_ATTR IRAM_ATTR
#define AUDIO_DRAM_ATTR DRAM_ATTR
#else
#define AUDIO_IRAM_ATTR
#define AUDIO_DRAM_ATTR
#endif

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

typedef struct {
    bool requested;
    bool held;
} audio_out_amp_t;

static inline bool
audio_out_amp_update(audio_out_amp_t* state, bool hold, bool enabled) {
    if (hold) {
        state->held = enabled;
    } else {
        state->requested = enabled;
    }
    return state->requested && !state->held;
}

bool audio_out_open(void);
/* Output belongs to the mixer for the lifetime of the firmware. */
void audio_out_close(void);
